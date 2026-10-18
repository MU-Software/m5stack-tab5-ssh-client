// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <esp_app_desc.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_private/panic_internal.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <riscv/rvruntime-frames.h>

#define KEPT        0x43524153 // "CRAS"
#define CANDIDATES  12
#define STACK_WORDS 256

typedef struct {
    uint32_t kept, core, mepc, ra, sp, mcause, mtval, code[CANDIDATES];
    int64_t up_us;
    char task[16], reason[40], description[48], abort[120], elf[9];
} report;

static const char* TAG = "crash";
static RTC_NOINIT_ATTR report kept;
static report last;
// Read at boot: flash may be out of reach in the panic handler
static char elf[9];
// The linker's names for where code lies
extern char _iram_text_start[], _iram_text_end[], _text_start[], _text_end[]; // NOLINT(bugprone-reserved-identifier)

static bool code(uint32_t address) {
    const char* p = (const char*)address;
    return (p >= _iram_text_start && p < _iram_text_end) || (p >= _text_start && p < _text_end);
}

// NOLINTBEGIN(bugprone-reserved-identifier): --wrap names them
void __real_esp_panic_handler(panic_info_t* info);

void __wrap_esp_panic_handler(panic_info_t* info) {
    const RvExcFrame* f = info->frame;
    TaskHandle_t task = xTaskGetCurrentTaskHandleForCore(info->core);
    kept = (report){.core = (uint32_t)info->core, .up_us = esp_timer_get_time()};
    if (f != NULL) kept.mepc = f->mepc, kept.ra = f->ra, kept.sp = f->sp, kept.mcause = f->mcause, kept.mtval = f->mtval;
    if (task != NULL) strlcpy(kept.task, pcTaskGetName(task), sizeof kept.task);
    strlcpy(kept.reason, info->reason != NULL ? info->reason : "", sizeof kept.reason);
    strlcpy(kept.description, info->description != NULL ? info->description : "", sizeof kept.description);
    strlcpy(kept.abort, g_panic_abort && g_panic_abort_details != NULL ? g_panic_abort_details : "", sizeof kept.abort);
    memcpy(kept.elf, elf, sizeof kept.elf);
    // Return addresses among the words of the stack: no frame pointers to walk
    const uint32_t* stack = (const uint32_t*)kept.sp;
    for (int i = 0, n = 0; f != NULL && n < CANDIDATES && i < STACK_WORDS && esp_ptr_byte_accessible(stack + i); i++)
        if (code(stack[i]) && stack[i] != kept.ra) kept.code[n++] = stack[i];
    kept.kept = KEPT;
    __real_esp_panic_handler(info);
}
// NOLINTEND(bugprone-reserved-identifier)

static void log_report(void) {
    char addresses[11 * (2 + CANDIDATES)] = "";
    // Return addresses less one: the line of the call, not of what follows it (after a call that never returns, another function)
    size_t n = (size_t)snprintf(addresses, sizeof addresses, "0x%08" PRIx32 " 0x%08" PRIx32, last.mepc, last.ra - 1);
    for (int i = 0; i < CANDIDATES && last.code[i] != 0 && n < sizeof addresses; i++)
        n += (size_t)snprintf(addresses + n, sizeof addresses - n, " 0x%08" PRIx32, last.code[i] - 1);
    ESP_LOGE(TAG,
             "core %" PRIu32 ", task %s, %s%s%s up %lld s, ELF %s: mepc 0x%08" PRIx32 " ra 0x%08" PRIx32 " sp 0x%08" PRIx32 " mcause 0x%" PRIx32
             " mtval 0x%08" PRIx32,
             last.core, last.task[0] ? last.task : "?", last.reason, last.description[0] ? ", " : "", last.description, last.up_us / 1000000,
             last.elf, last.mepc, last.ra, last.sp, last.mcause, last.mtval);
    if (last.abort[0]) ESP_LOGE(TAG, "abort: %s", last.abort);
    ESP_LOGE(TAG, "make decode ADDRESSES=\"%s\"", addresses);
}

void crash_start(void) {
    esp_reset_reason_t why = esp_reset_reason();
    esp_app_get_elf_sha256(elf, sizeof elf);
    if (kept.kept == KEPT && (why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT)) {
        last = kept;
        log_report();
    }
    kept.kept = 0;
}

void crash_command(void) {
    if (last.kept == KEPT)
        log_report();
    else
        ESP_LOGI(TAG, "no crash before this boot");
}
