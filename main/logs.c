// SPDX-License-Identifier: GPL-3.0-or-later
#include "logs.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include "clock.h"
#include "storage.h"

#define DIRECTORY SD "/.local/state/tab5-ssh/log"
#define RING      (64 * 1024)
#define FILES     512

static const char* TAG = "logs";
static char* ring;
static size_t head, tail, lost; // head - tail bytes wait; counts that only grow
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t console;
static log_settings settings;
static atomic_bool configured;
static FILE* file;
static char name[24];
static long size;
static char* buffer; // whole 4 KiB sectors per write, as the Stamp CAT-M research logger found

static int hook(const char* format, va_list args) {
    char line[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof line, format, copy);
    va_end(copy);
    if (n >= (int)sizeof line) line[(n = sizeof line - 1) - 1] = '\n';
    taskENTER_CRITICAL(&guard);
    for (int i = 0; i < n; i++) {
        if (head - tail == RING) tail++, lost++;
        ring[head++ % RING] = line[i];
    }
    taskEXIT_CRITICAL(&guard);
    return console(format, args);
}

static size_t take(char* out, size_t max) {
    size_t n = 0;
    taskENTER_CRITICAL(&guard);
    for (; n < max && tail != head; n++)
        out[n] = ring[tail++ % RING];
    taskEXIT_CRITICAL(&guard);
    return n;
}

static void today(char date[11]) {
    struct tm t;
    time_t now = time(NULL);
    strlcpy(date, "0000-00-00", 11);
    if (clock_known() && localtime_r(&now, &t) != NULL) strftime(date, 11, "%Y-%m-%d", &t);
}

static int by_name(const void* a, const void* b) {
    return strcmp(a, b);
}

// Oldest first, as the names sort, while over keep_mb or, for dated files, older than keep_days
static void prune(void) {
    static char names[FILES][24];
    char path[96], cutoff[11] = "";
    struct stat st;
    size_t n = 0;
    long long total = 0;
    DIR* d = opendir(DIRECTORY);
    if (d == NULL) return;
    for (const struct dirent* e; n < FILES && (e = readdir(d)) != NULL;) {
        size_t length = strlen(e->d_name);
        if (length < sizeof names[0] && length > 4 && strcmp(e->d_name + length - 4, ".log") == 0) strlcpy(names[n++], e->d_name, sizeof names[0]);
    }
    closedir(d);
    qsort(names, n, sizeof names[0], by_name);
    for (size_t i = 0; i < n; i++) {
        snprintf(path, sizeof path, DIRECTORY "/%.23s", names[i]);
        if (stat(path, &st) == 0) total += st.st_size;
    }
    if (clock_known()) {
        struct tm t;
        time_t then = time(NULL) - (time_t)settings.keep_days * 86400;
        if (localtime_r(&then, &t) != NULL) strftime(cutoff, sizeof cutoff, "%Y-%m-%d", &t);
    }
    for (size_t i = 0; i < n && strcmp(names[i], name) != 0; i++) {
        bool old = cutoff[0] && strncmp(names[i], "0000", 4) != 0 && strncmp(names[i], cutoff, 10) < 0;
        if (!old && total <= (long long)settings.keep_mb << 20) break;
        snprintf(path, sizeof path, DIRECTORY "/%.23s", names[i]);
        if (stat(path, &st) == 0 && unlink(path) == 0) total -= st.st_size;
    }
}

// Today's last file, or the next one once it is full
static void open_file(const char* date) {
    char path[96];
    struct stat st;
    if (file != NULL) fclose(file);
    file = NULL;
    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof name, "%s-%02d.log", date, i);
        snprintf(path, sizeof path, DIRECTORY "/%s", name);
        bool exists = stat(path, &st) == 0;
        if (exists && st.st_size >= (long)settings.file_kb * 1024) continue;
        snprintf(path, sizeof path, DIRECTORY "/%s-%02d.log", date, i + 1);
        if (stat(path, &st) == 0) continue; // a later one exists: today's last is further on
        snprintf(path, sizeof path, DIRECTORY "/%s", name);
        if ((file = fopen(path, "a")) == NULL) break;
        setvbuf(file, buffer, _IOFBF, 8192);
        size = (exists ? st.st_size : 0) + fprintf(file, "# tab5-ssh %s, reset reason %d, up %lld s\n", esp_app_get_description()->version,
                                                   esp_reset_reason(), esp_timer_get_time() / 1000000);
        break;
    }
    prune();
}

static void writer(void* arg) {
    (void)arg;
    static char chunk[4096];
    size_t lost_seen = 0;
    int64_t synced = 0;
    bool failed = false;
    for (;; vTaskDelay(pdMS_TO_TICKS(1000))) {
        char date[11];
        if (!configured || !settings.enabled) continue;
        today(date);
        if (file == NULL || strncmp(name, date, 10) != 0 || size >= (long)settings.file_kb * 1024) open_file(date);
        if (file == NULL) {
            if (!failed) ESP_LOGW(TAG, "no log file on the SD card");
            failed = true;
            continue;
        }
        failed = false;
        if (lost != lost_seen) {
            size += fprintf(file, "[%u bytes of log lost]\n", (unsigned)(lost - lost_seen));
            lost_seen = lost;
        }
        for (size_t n; (n = take(chunk, sizeof chunk)) > 0; size += (long)n)
            if (fwrite(chunk, 1, n, file) != n) {
                fclose(file);
                file = NULL;
                break;
            }
        // A power cut loses at most the last 2 s
        if (file != NULL && esp_timer_get_time() - synced >= 2000000) {
            fflush(file);
            fsync(fileno(file));
            synced = esp_timer_get_time();
        }
    }
}

esp_err_t logs_start(void) {
    if ((ring = heap_caps_malloc(RING, MALLOC_CAP_SPIRAM)) == NULL || (buffer = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM)) == NULL)
        return ESP_ERR_NO_MEM;
    console = esp_log_set_vprintf(hook);
    // Stack in PSRAM: a task there must never write flash
    return xTaskCreatePinnedToCoreWithCaps(writer, "logs", 4096, NULL, 4, NULL, 1, MALLOC_CAP_SPIRAM) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void logs_configure(const log_settings* s) {
    settings = *s;
    mkdir(SD "/.local", 0700);
    mkdir(SD "/.local/state", 0700);
    mkdir(SD "/.local/state/tab5-ssh", 0700);
    mkdir(DIRECTORY, 0700);
    configured = true;
}
