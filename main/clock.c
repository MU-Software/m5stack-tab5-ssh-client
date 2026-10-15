// SPDX-License-Identifier: GPL-3.0-or-later
#include "clock.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <bsp/m5stack_tab5.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <freertos/FreeRTOS.h>

#define RX8130 0x32
#define TIME   0x10 // seconds to years, BCD, years from 2000; held in UTC here
#define FLAG   0x1D
#define VLF    0x02 // the backup supply ran low: the time is lost
#define CTRL0  0x1E
#define STOP   0x40

static const char* TAG = "clock";
static i2c_master_dev_handle_t rtc;
static atomic_bool known;
static TaskHandle_t saver;

static int bcd(uint8_t v) {
    return (v >> 4) * 10 + (v & 0x0f);
}

static uint8_t to_bcd(int v) {
    return (uint8_t)((v / 10) << 4 | v % 10);
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil)
static int64_t days(int year, int month, int day) {
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int yoe = (int)(year - era * 400), doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

static esp_err_t rtc_write(const uint8_t* bytes, size_t size) {
    return i2c_master_transmit(rtc, bytes, size, 100);
}

static esp_err_t rtc_read(uint8_t reg, uint8_t* out, size_t size) {
    return i2c_master_transmit_receive(rtc, &reg, 1, out, size, 100);
}

// A clock nobody set since the factory reads a date years off without losing power, hence the bounds
static void from_rtc(void) {
    uint8_t flag = 0, t[7];
    static const char build[] = __DATE__; // "Mmm dd yyyy"
    int year_built = atoi(build + 7);
    if (rtc_read(FLAG, &flag, 1) != ESP_OK || rtc_read(TIME, t, sizeof t) != ESP_OK) return;
    int year = 2000 + bcd(t[6]);
    if (flag & VLF || year < year_built || year > year_built + 10) {
        ESP_LOGW(TAG, "RTC holds no time");
        return;
    }
    time_t now =
        (time_t)(days(year, bcd(t[5] & 0x1f), bcd(t[4] & 0x3f)) * 86400 + bcd(t[2] & 0x3f) * 3600 + bcd(t[1] & 0x7f) * 60 + bcd(t[0] & 0x7f));
    settimeofday(&(struct timeval){.tv_sec = now}, NULL);
    known = true;
}

static void to_rtc(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        time_t now = time(NULL);
        struct tm t;
        uint8_t ctrl0 = 0, flag = 0;
        gmtime_r(&now, &t);
        uint8_t frame[] = {TIME,
                           to_bcd(t.tm_sec),
                           to_bcd(t.tm_min),
                           to_bcd(t.tm_hour),
                           (uint8_t)(1 << t.tm_wday),
                           to_bcd(t.tm_mday),
                           to_bcd(t.tm_mon + 1),
                           to_bcd(t.tm_year % 100)};
        // Stopped, the seven registers cannot roll over mid-write
        if (rtc_read(CTRL0, &ctrl0, 1) != ESP_OK || rtc_write((uint8_t[]){CTRL0, ctrl0 | STOP}, 2) != ESP_OK ||
            rtc_write(frame, sizeof frame) != ESP_OK || rtc_write((uint8_t[]){CTRL0, ctrl0 & ~STOP}, 2) != ESP_OK ||
            rtc_read(FLAG, &flag, 1) != ESP_OK || rtc_write((uint8_t[]){FLAG, flag & ~VLF}, 2) != ESP_OK)
            ESP_LOGW(TAG, "RTC not set");
    }
}

// On lwIP's task, with little stack
static void synced(struct timeval* tv) {
    (void)tv;
    known = true;
    if (rtc != NULL) xTaskNotifyGive(saver);
}

esp_err_t clock_start(void) {
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &(i2c_device_config_t){.device_address = RX8130, .scl_speed_hz = 400000}, &rtc) != ESP_OK ||
        xTaskCreatePinnedToCore(to_rtc, "clock", 3072, NULL, 3, &saver, 1) != pdPASS) {
        ESP_LOGE(TAG, "no RTC");
        rtc = NULL;
    } else {
        from_rtc();
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = synced;
    return esp_netif_sntp_init(&config);
}

void clock_timezone(const char* timezone) {
    setenv("TZ", timezone, 1);
    tzset();
}

bool clock_known(void) {
    return known;
}
