#include "QemuAcceptance.h"

#ifdef CROSSINK_QEMU

#include <Arduino.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <QemuHalControl.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "MappedInputManager.h"

namespace {
constexpr char SENTINEL_PATH[] = "/qemu/sentinel.txt";
constexpr char SENTINEL_CONTENT[] = "crossink-qemu-sentinel-v1\n";
constexpr uint32_t EXPECTED_FRAME_BYTES = 48000;
constexpr uint32_t EXPECTED_FRAME_CRC32 = 0x0F7C8C45;
constexpr uint32_t NO_ALLOCATION_SAMPLE = 0;

enum class AcceptancePhase : uint8_t { NotStarted, WaitingForPowerSaving, Failed, Finished };

struct AcceptanceState {
  AcceptancePhase phase = AcceptancePhase::NotStarted;
  uint32_t idleStartedAt = 0;
  uint32_t heapStart = 0;
  uint32_t minFreeHeap = 0;
  uint32_t minMaxAllocation = 0;
  uint32_t minStackMargin = 0;
};

AcceptanceState state;

void fail(const char* component, const char* reason) {
  esp_rom_printf("QEMU_%s_FAIL reason=%s\n", component, reason);
  state.phase = AcceptancePhase::Failed;
}

uint32_t stackMarginBytes() {
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) * static_cast<uint32_t>(sizeof(StackType_t));
}

void sampleRuntime() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t reportedMinimum = ESP.getMinFreeHeap();
  const uint32_t maxAllocation = ESP.getMaxAllocHeap();
  const uint32_t stackMargin = stackMarginBytes();

  if (freeHeap < state.minFreeHeap) {
    state.minFreeHeap = freeHeap;
  }
  if (reportedMinimum < state.minFreeHeap) {
    state.minFreeHeap = reportedMinimum;
  }
  if (maxAllocation < state.minMaxAllocation) {
    state.minMaxAllocation = maxAllocation;
  }
  if (stackMargin < state.minStackMargin) {
    state.minStackMargin = stackMargin;
  }
}

bool checkStorage() {
  const uint32_t opensBefore = QemuHalControl::storageOpenCount();
  const uint32_t closesBefore = QemuHalControl::storageCloseCount();
  FsFile sentinel = Storage.open(SENTINEL_PATH, O_RDONLY);
  if (!sentinel) {
    fail("STORAGE", "open");
    return false;
  }

  char bytes[sizeof(SENTINEL_CONTENT) - 1];
  const uint64_t fileSize = sentinel.fileSize64();
  const int bytesRead = sentinel.read(bytes, sizeof(bytes));
  const bool closed = sentinel.close();
  const bool countersBalanced = QemuHalControl::storageOpenCount() == opensBefore + 1 &&
                                QemuHalControl::storageCloseCount() == closesBefore + 1;

  if (fileSize != sizeof(bytes) || bytesRead != static_cast<int>(sizeof(bytes)) || !closed || !countersBalanced ||
      std::memcmp(bytes, SENTINEL_CONTENT, sizeof(bytes)) != 0) {
    fail("STORAGE", "content");
    return false;
  }

  esp_rom_printf("QEMU_STORAGE_PASS path=%s bytes=%lu\n", SENTINEL_PATH,
                 static_cast<unsigned long>(sizeof(bytes)));
  return true;
}

bool checkFrame() {
  if (display.getBufferSize() != EXPECTED_FRAME_BYTES || display.getFrameBuffer() == nullptr) {
    fail("FRAME", "buffer");
    return false;
  }

  display.clearScreen(0xFF);
  display.getFrameBuffer()[0] = 0x7F;
  display.displayBuffer(HalDisplay::FAST_REFRESH);
  const uint32_t crc32 = QemuHalControl::frameCrc32();
  if (crc32 != EXPECTED_FRAME_CRC32) {
    fail("FRAME", "crc32");
    return false;
  }

  esp_rom_printf("QEMU_FRAME_PASS bytes=%lu crc32=%08lX\n", static_cast<unsigned long>(display.getBufferSize()),
                 static_cast<unsigned long>(crc32));
  return true;
}

bool checkInput(MappedInputManager& input) {
  constexpr MappedInputManager::Button BUTTON = MappedInputManager::Button::Down;
  input.simulatorInjectPress(BUTTON);
  const bool pressObserved = input.wasPressed(BUTTON);
  input.simulatorClearInputFrame();
  input.simulatorInjectRelease(BUTTON);
  const bool releaseObserved = input.wasReleased(BUTTON);
  input.simulatorClearInputFrame();

  if (!pressObserved || !releaseObserved || input.isPressed(BUTTON)) {
    fail("INPUT", "logical_down");
    return false;
  }

  esp_rom_printf("QEMU_INPUT_PASS button=DOWN press=1 release=1\n");
  return true;
}
}  // namespace

void qemuAcceptanceBegin(MappedInputManager& input) {
  state = {};
  esp_rom_printf("QEMU_BOOT seq=0\n");

  if (!checkStorage() || !checkFrame() || !checkInput(input)) {
    return;
  }

  state.heapStart = ESP.getFreeHeap();
  state.minFreeHeap = state.heapStart;
  state.minMaxAllocation = ESP.getMaxAllocHeap();
  state.minStackMargin = stackMarginBytes();
  state.idleStartedAt = millis();
  state.phase = AcceptancePhase::WaitingForPowerSaving;
  sampleRuntime();
}

void qemuAcceptanceTick() {
  if (state.phase != AcceptancePhase::WaitingForPowerSaving) {
    return;
  }

  sampleRuntime();
  if (millis() - state.idleStartedAt < HalPowerManager::IDLE_POWER_SAVING_MS ||
      !QemuHalControl::powerSavingEnabled()) {
    return;
  }

  esp_rom_printf("QEMU_POWER_PASS idle_ms=%lu saving=1\n",
                 static_cast<unsigned long>(HalPowerManager::IDLE_POWER_SAVING_MS));
  esp_rom_printf(
      "QEMU_RUNTIME heap_start=%lu min_free=%lu min_max_alloc=%lu max_alloc=%lu stack_margin=%lu\n",
      static_cast<unsigned long>(state.heapStart), static_cast<unsigned long>(state.minFreeHeap),
      static_cast<unsigned long>(state.minMaxAllocation), static_cast<unsigned long>(NO_ALLOCATION_SAMPLE),
      static_cast<unsigned long>(state.minStackMargin));
  esp_rom_printf("QEMU_TRACER_PASS\n");
  state.phase = AcceptancePhase::Finished;
}

#endif
