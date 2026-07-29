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
