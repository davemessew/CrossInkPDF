#pragma once

#include <ReflowDocument.h>

#include <cstdint>

#include "PdfCacheIo.h"
#include "PdfSourceIdentity.h"

class PdfProgressStore {
 public:
  PdfStatus initialize(const PdfCacheIo& io, const char* cacheRoot, const PdfSourceIdentity& source,
                       uint32_t totalWords);
  PdfStatus load(ReflowReadingPosition* position) const;
 PdfStatus save(const ReflowReadingPosition& position) const;

 private:
  struct SlotRecord;
  enum class PendingSlot : uint8_t {
    None,
    A,
    B,
  };

  PdfStatus readSlot(const char* name, SlotRecord* record) const;
  PdfStatus writeSlot(const char* name, const SlotRecord& record) const;
  bool matchesIdentity(const SlotRecord& record) const;
  bool formatPath(const char* name, char destination[PDF_CACHE_PATH_CAPACITY]) const;

  PdfCacheIo io_{};
  const char* cacheRoot_ = nullptr;
  PdfSourceIdentity source_{};
  uint32_t totalWords_ = 0;
  bool initialized_ = false;
  mutable PendingSlot pendingSlot_ = PendingSlot::None;
  mutable uint32_t pendingSequence_ = 0;
};
