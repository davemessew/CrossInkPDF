#pragma once

#include <cstdint>

#include "ReflowDocument.h"

enum class ReflowReaderSyncAction : uint8_t {
  ExternalProgress,
  NearbyProgress,
};

constexpr bool reflowSupportsSyncAction(const ReflowCapabilitySet capabilities, const ReflowReaderSyncAction action) {
  switch (action) {
    case ReflowReaderSyncAction::ExternalProgress:
      return hasReflowCapability(capabilities, ReflowCapability::ExternalProgressSync);
    case ReflowReaderSyncAction::NearbyProgress:
      return hasReflowCapability(capabilities, ReflowCapability::NearbyProgressSync);
  }
  return false;
}

constexpr bool reflowSupportsMenuAction(const ReflowCapabilitySet capabilities, const ReflowReaderSyncAction action) {
  return reflowSupportsSyncAction(capabilities, action);
}

constexpr bool reflowSupportsQuickAction(const ReflowCapabilitySet capabilities, const ReflowReaderSyncAction action) {
  return reflowSupportsSyncAction(capabilities, action);
}

constexpr bool reflowUsesPublisherRenderModes(const ReflowCapabilitySet capabilities) {
  return hasReflowCapability(capabilities, ReflowCapability::PublisherRenderModes);
}

constexpr bool reflowUsesEmbeddedStyles(const ReflowCapabilitySet capabilities) {
  return hasReflowCapability(capabilities, ReflowCapability::EmbeddedStyles);
}

constexpr bool reflowSupportsSavedItems(const ReflowCapabilitySet capabilities) {
  return hasReflowCapability(capabilities, ReflowCapability::SavedItems);
}

constexpr int reflowPageForRelayout(const int oldPage, const int oldPageCount, const int newPageCount) {
  if (oldPageCount <= 0 || newPageCount <= 0 || oldPage <= 0) {
    return 0;
  }

  const int clampedOldPage = oldPage < oldPageCount ? oldPage : oldPageCount - 1;
  const uint64_t mapped =
      static_cast<uint64_t>(clampedOldPage) * static_cast<uint64_t>(newPageCount) / static_cast<uint64_t>(oldPageCount);
  return mapped < static_cast<uint64_t>(newPageCount) ? static_cast<int>(mapped) : newPageCount - 1;
}
