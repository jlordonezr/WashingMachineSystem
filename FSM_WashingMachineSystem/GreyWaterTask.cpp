// FreeRTOS parts of the safety design (board only; the PC simulation has its
// own stand-ins in tests/sim/test_cycle.cpp). See GreyWaterOutputs.h.
#include "GreyWaterOutputs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define SAFETY_TASK_PRIORITY 3      // above loop() (1); below the Wi-Fi tasks on core 0
#define SAFETY_TASK_CORE     1      // same core as loop(), so the lock is never contended across cores for long
#define SAFETY_TASK_STACK    3072

static SemaphoreHandle_t lock = nullptr;

static SemaphoreHandle_t theLock() {
  // Created on first use: outputsBegin() runs in setup(), before safetyBegin().
  if (!lock) lock = xSemaphoreCreateRecursiveMutex();
  return lock;
}

void gwLock()   { xSemaphoreTakeRecursive(theLock(), portMAX_DELAY); }
void gwUnlock() { xSemaphoreGiveRecursive(theLock()); }

static void safetyTask(void*) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    safetyTick();
    vTaskDelayUntil(&last, pdMS_TO_TICKS(SAFETY_TICK_MS));
  }
}

void safetyBegin() {
  theLock();
  xTaskCreatePinnedToCore(safetyTask, "safety", SAFETY_TASK_STACK, nullptr, SAFETY_TASK_PRIORITY, nullptr, SAFETY_TASK_CORE);
}
