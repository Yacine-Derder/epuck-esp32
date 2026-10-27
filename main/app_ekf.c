#include "app_ekf.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#ifndef BRIDGE_HOST_TEST
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static TaskHandle_t worker;
#endif
#define SLOTS 8u
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "EKF queues require lock-free unsigned atomics");
typedef struct {
  uint32_t generation, receivedMs;
  char line[EKF_SERVICE_LINE];
} message_t;
typedef struct {
  message_t slots[SLOTS];
  atomic_uint head, tail;
} ring_t;
static ring_t requests, replies;
static atomic_uint desiredGeneration, dropped;
static uint32_t ownedGeneration;
static ekf_service_t service;
static bool push(ring_t *q, const message_t *m) {
  unsigned head = atomic_load_explicit(&q->head, memory_order_relaxed);
  unsigned tail = atomic_load_explicit(&q->tail, memory_order_acquire);
  if (head - tail == SLOTS)
    return false;
  q->slots[head % SLOTS] = *m;
  atomic_store_explicit(&q->head, head + 1, memory_order_release);
  return true;
}
static bool pop(ring_t *q, message_t *m) {
  unsigned tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
  unsigned head = atomic_load_explicit(&q->head, memory_order_acquire);
  if (tail == head)
    return false;
  *m = q->slots[tail % SLOTS];
  atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
  return true;
}
static void synchronize(void) {
  unsigned next = atomic_load_explicit(&desiredGeneration, memory_order_acquire);
  if (next != ownedGeneration) {
    ekf_service_clear(&service);
    ownedGeneration = next;
  }
}
#ifdef BRIDGE_HOST_TEST
bool app_ekf_worker_step(void)
#else
static bool app_ekf_worker_step(void)
#endif
{
  synchronize();
  message_t input, output = {0};
  if (!pop(&requests, &input))
    return false;
  // Recheck after dequeue; a disconnect can occur while a command is running.
  synchronize();
  if (input.generation != ownedGeneration)
    return true;
  output.generation = ownedGeneration;
  output.receivedMs = 0;
  bool handled;
  if (!strcmp(input.line, "EKF WORKER")) {
#ifdef BRIDGE_HOST_TEST
    snprintf(output.line, sizeof(output.line),
             "EKF1 type=worker stack_bytes=8192 stack_min_free_bytes=UNKNOWN "
             "heap_free_8bit=UNKNOWN heap_largest_8bit=UNKNOWN dropped=%" PRIu32,
             app_ekf_dropped());
#else
    snprintf(output.line, sizeof(output.line),
             "EKF1 type=worker stack_bytes=8192 stack_min_free_bytes=%u heap_free_8bit=%zu "
             "heap_largest_8bit=%zu dropped=%" PRIu32,
             (unsigned)uxTaskGetStackHighWaterMark(NULL), heap_caps_get_free_size(MALLOC_CAP_8BIT),
             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), app_ekf_dropped());
#endif
    handled = true;
  } else {
    handled = ekf_service_command(&service, input.line, input.receivedMs, output.line);
  }
  if (handled && !push(&replies, &output))
    atomic_fetch_add(&dropped, 1);
  return true;
}
#ifndef BRIDGE_HOST_TEST
static void run(void *unused) {
  (void)unused;
  for (;;) {
    unsigned budget = 0;
    while (app_ekf_worker_step()) {
      // Continuous optional input must still let the idle/watchdog tasks run.
      if (++budget == 4) {
        vTaskDelay(1);
        budget = 0;
      }
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  }
}
#endif
void app_ekf_reset(uint32_t generation) {
  atomic_store_explicit(&desiredGeneration, generation, memory_order_release);
#ifndef BRIDGE_HOST_TEST
  if (worker)
    xTaskNotifyGive(worker);
#endif
}
bool app_ekf_submit(uint32_t generation, const char *line, uint32_t receivedMs) {
  if (!line || strlen(line) >= EKF_SERVICE_LINE)
    return false;
#ifndef BRIDGE_HOST_TEST
  // Start only on explicit EKF use; compute runs below bridge/control priority5.
  if (!worker && xTaskCreate(run, "mul_ekf", 8192, NULL, 3, &worker) != pdPASS)
    return false;
#endif
  message_t m = {.generation = generation, .receivedMs = receivedMs};
  strcpy(m.line, line);
  if (!push(&requests, &m)) {
    atomic_fetch_add(&dropped, 1);
    return false;
  }
#ifndef BRIDGE_HOST_TEST
  xTaskNotifyGive(worker);
#endif
  return true;
}
bool app_ekf_receive(uint32_t *generation, char line[EKF_SERVICE_LINE]) {
  message_t m;
  if (!pop(&replies, &m))
    return false;
  *generation = m.generation;
  memcpy(line, m.line, sizeof(m.line));
  return true;
}
uint32_t app_ekf_dropped(void) { return atomic_load(&dropped); }
