#include "PdfSavedItemsSession.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>

namespace {

constexpr uint16_t kInvalidIndex = UINT16_MAX;
constexpr uint16_t kFirstItemId = 1;
constexpr uint16_t kLastItemId = UINT16_MAX - 1U;
static_assert(PDF_SAVED_ITEMS_MAX_BOOKMARKS == PDF_SAVED_ITEMS_MAX_CLIPPINGS);

uint16_t nextUsableItemId(const uint16_t value) {
  return value >= kLastItemId ? kFirstItemId : static_cast<uint16_t>(value + 1U);
}

bool maskBit(const uint8_t* const mask, const uint16_t index) {
  return (mask[index / 8U] & static_cast<uint8_t>(1U << (index % 8U))) != 0;
}

void setMaskBit(uint8_t* const mask, const uint16_t index, const bool value) {
  const uint8_t bit = static_cast<uint8_t>(1U << (index % 8U));
  if (value) {
    mask[index / 8U] |= bit;
  } else {
    mask[index / 8U] &= static_cast<uint8_t>(~bit);
  }
}

}  // namespace

PdfStatus PdfSavedItemsSession::begin(PdfSavedItemsBuffer* const buffer, const PdfSavedItemsPersistence& persistence,
                                      const PdfSavedItemsLegacyAccess& bookmarks,
                                      const PdfSavedItemsLegacyAccess& clippings) {
  buffer_ = buffer;
  persistence_ = persistence;
  bookmarks_ = bookmarks;
  clippings_ = clippings;
  nextItemId_ = kFirstItemId;
  pendingItemId_ = 0;
  capabilities_ = 0;
  initialized_ = false;
  dirty_ = false;
  hasPendingJump_ = false;

  if (buffer_ == nullptr || buffer_->items == nullptr || buffer_->capacity < PDF_SAVED_ITEMS_MAX_RECORDS ||
      !persistence_.valid()) {
    if (buffer_ != nullptr) buffer_->count = 0;
    return PdfStatus::failure(PdfError::InvalidArgument);
  }

  buffer_->count = 0;
  const PdfStatus loadStatus = persistence_.load(persistence_.context, buffer_);
  if (!loadStatus && loadStatus.error != PdfError::InvalidOffset) {
    buffer_->count = 0;
    return loadStatus;
  }
  if (!loadStatus) buffer_->count = 0;
  if (buffer_->count > PDF_SAVED_ITEMS_MAX_RECORDS || buffer_->count > buffer_->capacity) {
    buffer_->count = 0;
    return PdfStatus::failure(PdfError::LimitExceeded);
  }

  initialized_ = true;
  if (probeLegacy(PdfSavedItemKind::Bookmark)) {
    capabilities_ |= PDF_SAVED_ITEMS_CAP_BOOKMARKS;
  }
  if (probeLegacy(PdfSavedItemKind::Clipping)) {
    capabilities_ |= PDF_SAVED_ITEMS_CAP_CLIPPINGS;
  }

  reconcile();
  if ((capabilities_ & (PDF_SAVED_ITEMS_CAP_BOOKMARKS | PDF_SAVED_ITEMS_CAP_CLIPPINGS)) ==
      (PDF_SAVED_ITEMS_CAP_BOOKMARKS | PDF_SAVED_ITEMS_CAP_CLIPPINGS)) {
    capabilities_ |= PDF_SAVED_ITEMS_CAP_ID_ALLOCATION;
    initializeNextItemId();
  }
  return PdfStatus::success();
}

bool PdfSavedItemsSession::validKind(const PdfSavedItemKind kind) const {
  return kind == PdfSavedItemKind::Bookmark || kind == PdfSavedItemKind::Clipping;
}

bool PdfSavedItemsSession::supports(const PdfSavedItemKind kind) const {
  return initialized_ && validKind(kind) && (capabilities_ & capabilityFor(kind)) != 0;
}

PdfSavedItemsLegacyAccess* PdfSavedItemsSession::accessFor(const PdfSavedItemKind kind) {
  if (kind == PdfSavedItemKind::Bookmark) return &bookmarks_;
  if (kind == PdfSavedItemKind::Clipping) return &clippings_;
  return nullptr;
}

const PdfSavedItemsLegacyAccess* PdfSavedItemsSession::accessFor(const PdfSavedItemKind kind) const {
  if (kind == PdfSavedItemKind::Bookmark) return &bookmarks_;
  if (kind == PdfSavedItemKind::Clipping) return &clippings_;
  return nullptr;
}

bool PdfSavedItemsSession::probeLegacy(const PdfSavedItemKind kind) {
  const PdfSavedItemsLegacyAccess* const access = accessFor(kind);
  if (access == nullptr || !access->mutableAccess()) return false;
  uint16_t legacyCount = 0;
  if (!access->count(access->context, &legacyCount) || legacyCount > PDF_SAVED_ITEMS_MAX_BOOKMARKS) {
    return false;
  }
  for (uint16_t index = 0; index < legacyCount; ++index) {
    uint16_t ignored = 0;
    if (!access->readItemId(access->context, index, &ignored)) return false;
  }
  return true;
}

bool PdfSavedItemsSession::legacyMatchCount(const PdfSavedItemsLegacyAccess& access, const uint16_t itemId,
                                            uint16_t* const matches) const {
  if (matches == nullptr) return false;
  *matches = 0;
  uint16_t legacyCount = 0;
  if (!access.count(access.context, &legacyCount) || legacyCount > PDF_SAVED_ITEMS_MAX_BOOKMARKS) return false;
  for (uint16_t index = 0; index < legacyCount; ++index) {
    uint16_t legacyItemId = 0;
    if (!access.readItemId(access.context, index, &legacyItemId)) return false;
    if (legacyItemId == itemId) ++*matches;
  }
  return true;
}

void PdfSavedItemsSession::maskCapability(const PdfSavedItemKind kind) {
  if (validKind(kind)) {
    capabilities_ &= static_cast<uint8_t>(~capabilityFor(kind));
  }
  capabilities_ &= static_cast<uint8_t>(~PDF_SAVED_ITEMS_CAP_ID_ALLOCATION);
}

void PdfSavedItemsSession::requireReload() {
  capabilities_ = 0;
  initialized_ = false;
  cancelPendingJump();
}

void PdfSavedItemsSession::reconcile() {
  if (buffer_ == nullptr) return;

  uint8_t keepMask[(PDF_SAVED_ITEMS_MAX_RECORDS + 7U) / 8U] = {};
  for (uint16_t index = 0; index < buffer_->count; ++index) {
    setMaskBit(keepMask, index, true);
  }

  for (const PdfSavedItemKind kind : {PdfSavedItemKind::Bookmark, PdfSavedItemKind::Clipping}) {
    if (!supports(kind)) continue;
    const PdfSavedItemsLegacyAccess* const access = accessFor(kind);
    bool readable = access != nullptr;
    for (uint16_t index = 0; readable && index < buffer_->count; ++index) {
      const PdfSavedItem& item = buffer_->items[index];
      if (item.kind != kind) continue;

      bool keep = item.itemId != 0 && item.itemId != UINT16_MAX;
      uint16_t duplicateCount = 0;
      for (uint16_t other = 0; keep && other < buffer_->count; ++other) {
        if (buffer_->items[other].itemId == item.itemId) ++duplicateCount;
      }
      keep = keep && duplicateCount == 1;

      uint16_t legacyMatches = 0;
      if (keep && !legacyMatchCount(*access, item.itemId, &legacyMatches)) {
        readable = false;
        break;
      }
      keep = keep && legacyMatches == 1;
      setMaskBit(keepMask, index, keep);
    }

    // PSIT is the semantic intent journal. Remove every legacy-only record so
    // a crash after a durable PSIT delete cannot resurrect a visible item.
    uint16_t legacyCount = 0;
    if (readable && !access->count(access->context, &legacyCount)) readable = false;
    uint16_t legacyIndex = 0;
    while (readable && legacyIndex < legacyCount) {
      uint16_t legacyItemId = 0;
      if (!access->readItemId(access->context, legacyIndex, &legacyItemId)) {
        readable = false;
        break;
      }

      bool retained = false;
      for (uint16_t psitIndex = 0; psitIndex < buffer_->count; ++psitIndex) {
        if (maskBit(keepMask, psitIndex) && buffer_->items[psitIndex].kind == kind &&
            buffer_->items[psitIndex].itemId == legacyItemId) {
          retained = true;
          break;
        }
      }
      if (retained) {
        ++legacyIndex;
        continue;
      }

      if (access->removeItem(access->context, legacyItemId) != PdfSavedItemsLegacyMutationResult::Applied) {
        readable = false;
        break;
      }
      uint16_t updatedCount = 0;
      if (!access->count(access->context, &updatedCount) || updatedCount >= legacyCount) {
        readable = false;
        break;
      }
      legacyCount = updatedCount;
    }

    if (!readable) {
      maskCapability(kind);
      for (uint16_t index = 0; index < buffer_->count; ++index) {
        if (buffer_->items[index].kind == kind) setMaskBit(keepMask, index, true);
      }
    }
  }

  uint16_t destination = 0;
  bool dropped = false;
  for (uint16_t source = 0; source < buffer_->count; ++source) {
    const PdfSavedItemKind kind = buffer_->items[source].kind;
    const bool knownKind = validKind(kind);
    const bool keep = knownKind && maskBit(keepMask, source);
    if (!keep) {
      dropped = true;
      continue;
    }
    if (destination != source) buffer_->items[destination] = buffer_->items[source];
    ++destination;
  }
  buffer_->count = destination;
  dirty_ = dirty_ || dropped;
}

void PdfSavedItemsSession::initializeNextItemId() {
  uint16_t highest = 0;
  for (uint16_t index = 0; index < buffer_->count; ++index) {
    const uint16_t itemId = buffer_->items[index].itemId;
    if (itemId != UINT16_MAX && itemId > highest) highest = itemId;
  }
  for (const PdfSavedItemsLegacyAccess* const access : {&bookmarks_, &clippings_}) {
    uint16_t legacyCount = 0;
    if (!access->count(access->context, &legacyCount)) {
      capabilities_ &= static_cast<uint8_t>(~PDF_SAVED_ITEMS_CAP_ID_ALLOCATION);
      return;
    }
    for (uint16_t index = 0; index < legacyCount; ++index) {
      uint16_t itemId = 0;
      if (!access->readItemId(access->context, index, &itemId)) {
        capabilities_ &= static_cast<uint8_t>(~PDF_SAVED_ITEMS_CAP_ID_ALLOCATION);
        return;
      }
      if (itemId != UINT16_MAX && itemId > highest) highest = itemId;
    }
  }
  nextItemId_ = nextUsableItemId(highest);
}

bool PdfSavedItemsSession::idUsed(const uint16_t itemId) {
  if (itemId == 0 || itemId == UINT16_MAX) return true;
  for (uint16_t index = 0; index < buffer_->count; ++index) {
    if (buffer_->items[index].itemId == itemId) return true;
  }
  for (const PdfSavedItemsLegacyAccess* const access : {&bookmarks_, &clippings_}) {
    uint16_t legacyMatches = 0;
    if (!legacyMatchCount(*access, itemId, &legacyMatches)) {
      capabilities_ &= static_cast<uint8_t>(~PDF_SAVED_ITEMS_CAP_ID_ALLOCATION);
      return true;
    }
    if (legacyMatches != 0) return true;
  }
  return false;
}

bool PdfSavedItemsSession::allocateItemId(uint16_t* const output) {
  if (output == nullptr || !canAllocateItemIds()) return false;
  uint16_t candidate = nextItemId_;
  for (uint32_t attempts = 0; attempts < kLastItemId; ++attempts) {
    if (!idUsed(candidate)) {
      *output = candidate;
      nextItemId_ = nextUsableItemId(candidate);
      return true;
    }
    if (!canAllocateItemIds()) return false;
    candidate = nextUsableItemId(candidate);
  }
  return false;
}

uint16_t PdfSavedItemsSession::count(const PdfSavedItemKind kind) const {
  if (!supports(kind) || buffer_ == nullptr) return 0;
  uint16_t result = 0;
  for (uint16_t index = 0; index < buffer_->count; ++index) {
    if (buffer_->items[index].kind == kind) ++result;
  }
  return result;
}

uint16_t PdfSavedItemsSession::findIndex(const PdfSavedItemKind kind, const uint16_t itemId) const {
  if (!supports(kind) || buffer_ == nullptr || itemId == 0 || itemId == UINT16_MAX) return kInvalidIndex;
  for (uint16_t index = 0; index < buffer_->count; ++index) {
    if (buffer_->items[index].kind == kind && buffer_->items[index].itemId == itemId) return index;
  }
  return kInvalidIndex;
}

const PdfSavedItem* PdfSavedItemsSession::find(const PdfSavedItemKind kind, const uint16_t itemId) const {
  const uint16_t index = findIndex(kind, itemId);
  return index == kInvalidIndex ? nullptr : &buffer_->items[index];
}

const PdfSavedItem* PdfSavedItemsSession::findBookmarkInPage(const uint16_t sectionIndex,
                                                             const uint32_t firstGlobalWordOrdinal,
                                                             const uint32_t lastGlobalWordOrdinal) const {
  if (!supports(PdfSavedItemKind::Bookmark) || buffer_ == nullptr || firstGlobalWordOrdinal > lastGlobalWordOrdinal) {
    return nullptr;
  }
  for (uint16_t index = 0; index < buffer_->count; ++index) {
    const PdfSavedItem& item = buffer_->items[index];
    if (item.kind == PdfSavedItemKind::Bookmark && item.sectionIndex == sectionIndex &&
        (item.flags & PDF_SAVED_ITEM_HAS_START_SEMANTIC) != 0 &&
        item.startGlobalWordOrdinal >= firstGlobalWordOrdinal && item.startGlobalWordOrdinal <= lastGlobalWordOrdinal) {
      return &item;
    }
  }
  return nullptr;
}

PdfSavedItemsSessionResult PdfSavedItemsSession::add(const PdfSavedItem& item, uint16_t* const assignedItemId) {
  if (!initialized_ || !validKind(item.kind)) return PdfSavedItemsSessionResult::InvalidArgument;
  if (!supports(item.kind) || !canAllocateItemIds()) return PdfSavedItemsSessionResult::Unavailable;
  PdfSavedItemsLegacyAccess* const access = accessFor(item.kind);
  if (access == nullptr || access->addItem == nullptr) return PdfSavedItemsSessionResult::Unavailable;

  uint16_t legacyCount = 0;
  if (!access->count(access->context, &legacyCount)) {
    maskCapability(item.kind);
    return PdfSavedItemsSessionResult::Unavailable;
  }
  const uint16_t kindLimit = PDF_SAVED_ITEMS_MAX_BOOKMARKS;
  if (legacyCount >= kindLimit || count(item.kind) >= kindLimit || buffer_->count >= buffer_->capacity ||
      buffer_->count >= PDF_SAVED_ITEMS_MAX_RECORDS) {
    return PdfSavedItemsSessionResult::LimitReached;
  }

  uint16_t itemId = 0;
  const uint16_t previousNextItemId = nextItemId_;
  if (!allocateItemId(&itemId)) return PdfSavedItemsSessionResult::Unavailable;

  PdfSavedItem saved = item;
  saved.itemId = itemId;
  if (!persistence_.validate(persistence_.context, saved)) {
    nextItemId_ = previousNextItemId;
    return PdfSavedItemsSessionResult::InvalidItem;
  }
  buffer_->items[buffer_->count++] = saved;
  dirty_ = true;
  if (!flush()) {
    requireReload();
    return PdfSavedItemsSessionResult::ReloadRequired;
  }

  const PdfSavedItemsLegacyMutationResult legacyResult = access->addItem(access->context, itemId);
  if (legacyResult == PdfSavedItemsLegacyMutationResult::Applied) {
    if (assignedItemId != nullptr) *assignedItemId = itemId;
    return PdfSavedItemsSessionResult::Applied;
  }
  if (legacyResult == PdfSavedItemsLegacyMutationResult::Ambiguous) {
    requireReload();
    return PdfSavedItemsSessionResult::ReloadRequired;
  }

  --buffer_->count;
  dirty_ = true;
  if (!flush()) {
    requireReload();
    return PdfSavedItemsSessionResult::ReloadRequired;
  }
  nextItemId_ = previousNextItemId;
  return PdfSavedItemsSessionResult::LegacyFailure;
}

PdfSavedItemsSessionResult PdfSavedItemsSession::remove(const PdfSavedItemKind kind, const uint16_t itemId) {
  if (!initialized_ || !validKind(kind)) return PdfSavedItemsSessionResult::InvalidArgument;
  if (!supports(kind)) return PdfSavedItemsSessionResult::Unavailable;
  const uint16_t index = findIndex(kind, itemId);
  if (index == kInvalidIndex) return PdfSavedItemsSessionResult::NotFound;
  PdfSavedItemsLegacyAccess* const access = accessFor(kind);
  if (access == nullptr || access->removeItem == nullptr) return PdfSavedItemsSessionResult::Unavailable;

  const PdfSavedItemsLegacyMutationResult legacyResult = access->removeItem(access->context, itemId);
  if (legacyResult == PdfSavedItemsLegacyMutationResult::Rejected) {
    return PdfSavedItemsSessionResult::LegacyFailure;
  }
  if (legacyResult == PdfSavedItemsLegacyMutationResult::Ambiguous) {
    requireReload();
    return PdfSavedItemsSessionResult::ReloadRequired;
  }

  for (uint16_t tail = index + 1; tail < buffer_->count; ++tail) {
    buffer_->items[tail - 1] = buffer_->items[tail];
  }
  --buffer_->count;
  dirty_ = true;
  if (!flush()) {
    requireReload();
    return PdfSavedItemsSessionResult::ReloadRequired;
  }
  if (hasPendingJump_ && pendingKind_ == kind && pendingItemId_ == itemId) cancelPendingJump();
  return PdfSavedItemsSessionResult::Applied;
}

PdfSavedItemsSessionResult PdfSavedItemsSession::clear(const PdfSavedItemKind kind) {
  if (!initialized_ || !validKind(kind)) return PdfSavedItemsSessionResult::InvalidArgument;
  if (!supports(kind)) return PdfSavedItemsSessionResult::Unavailable;
  PdfSavedItemsLegacyAccess* const access = accessFor(kind);
  if (access == nullptr || access->clearItems == nullptr) return PdfSavedItemsSessionResult::Unavailable;

  if (count(kind) == 0) return PdfSavedItemsSessionResult::Applied;
  const PdfSavedItemsLegacyMutationResult legacyResult = access->clearItems(access->context);
  if (legacyResult == PdfSavedItemsLegacyMutationResult::Rejected) {
    return PdfSavedItemsSessionResult::LegacyFailure;
  }
  if (legacyResult == PdfSavedItemsLegacyMutationResult::Ambiguous) {
    requireReload();
    return PdfSavedItemsSessionResult::ReloadRequired;
  }

  const uint16_t previousCount = buffer_->count;
  uint16_t keptCount = 0;
  for (uint16_t source = 0; source < previousCount; ++source) {
    if (buffer_->items[source].kind == kind) continue;
    if (keptCount != source) std::swap(buffer_->items[keptCount], buffer_->items[source]);
    ++keptCount;
  }
  buffer_->count = keptCount;
  dirty_ = true;
  if (!flush()) {
    requireReload();
    return PdfSavedItemsSessionResult::ReloadRequired;
  }
  if (hasPendingJump_ && pendingKind_ == kind) cancelPendingJump();
  return PdfSavedItemsSessionResult::Applied;
}

PdfStatus PdfSavedItemsSession::flush() {
  if (!initialized_) return PdfStatus::failure(PdfError::InvalidArgument);
  if (!dirty_) return PdfStatus::success();
  const PdfStatus status = persistence_.save(persistence_.context, buffer_->items, buffer_->count);
  if (status) dirty_ = false;
  return status;
}

PdfSavedItemsSessionResult PdfSavedItemsSession::queueJump(const PdfSavedItemKind kind, const uint16_t itemId) {
  if (!initialized_ || !validKind(kind)) return PdfSavedItemsSessionResult::InvalidArgument;
  if (!supports(kind)) return PdfSavedItemsSessionResult::Unavailable;
  if (find(kind, itemId) == nullptr) return PdfSavedItemsSessionResult::NotFound;
  pendingKind_ = kind;
  pendingItemId_ = itemId;
  hasPendingJump_ = true;
  return PdfSavedItemsSessionResult::Applied;
}

bool PdfSavedItemsSession::pendingJump(PdfSavedItem* const output) const {
  if (!hasPendingJump_ || output == nullptr) return false;
  const PdfSavedItem* const item = find(pendingKind_, pendingItemId_);
  if (item == nullptr) return false;
  *output = *item;
  return true;
}

void PdfSavedItemsSession::resolvePendingJump(const bool applied) {
  if (applied) cancelPendingJump();
}

void PdfSavedItemsSession::cancelPendingJump() {
  pendingItemId_ = 0;
  hasPendingJump_ = false;
}
