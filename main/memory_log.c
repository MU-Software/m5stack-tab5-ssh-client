// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory_log.h"

#include <stdatomic.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define DMA      (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)

static const char* TAG = "memory";
static atomic_uint failures;
static size_t failed_size;
static uint32_t failed_caps;
static char failed_task[configMAX_TASK_NAME_LEN];

// Inside the failing allocation: no logging, no allocating
static void failed(size_t size, uint32_t caps, const char* function) {
    const char* name = xPortInIsrContext() ? "ISR" : pcTaskGetName(NULL);
    (void)function;
    failed_size = size, failed_caps = caps;
    strncpy(failed_task, name != NULL ? name : "?", sizeof failed_task - 1);
    atomic_fetch_add(&failures, 1);
}

void memory_log_start(void) {
    heap_caps_register_failed_alloc_callback(failed);
}

void memory_log(void) {
    static unsigned reported;
    static int64_t last = -60 * 1000000LL;
    int64_t now = esp_timer_get_time();
    unsigned n = atomic_load(&failures);
    if (n != reported) {
        ESP_LOGW(TAG, "%u failed allocations, the last %u B caps 0x%lx in %s", n - reported, failed_size, (unsigned long)failed_caps, failed_task);
        reported = n;
    }
    if (now - last < 60 * 1000000LL) return;
    last = now;
    ESP_LOGI(TAG, "internal %u free, %u least; DMA %u free, %u in one block; PSRAM %u free", heap_caps_get_free_size(INTERNAL),
             heap_caps_get_minimum_free_size(INTERNAL), heap_caps_get_free_size(DMA), heap_caps_get_largest_free_block(DMA),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
