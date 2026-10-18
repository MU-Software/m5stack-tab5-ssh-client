// SPDX-License-Identifier: GPL-3.0-or-later
#include "sleep.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/pmu_ll.h>

#include "c6.h"
#include "keyboard_tab5.h"

static const char* TAG = "sleep";

// Each low while it has something: USB-C's D+ for a computer's bus reset or resume (no VBUS sensing, and the full-speed
// controller cannot wake the chip), SDIO D1 between transfers for the C6's interrupt
static const struct {
    gpio_num_t pin;
    sleep_wake wake;
    const char* name;
} sources[] = {
    {KEYBOARD_TAB5_INTERRUPT, SLEEP_KEYBOARD, " keyboard"},
    {GPIO_NUM_25, SLEEP_USB, " USB"},
    {CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D1, SLEEP_WIFI, " Wi-Fi"},
};

static atomic_uint holds, sequence;
static _Atomic int64_t held_until;

void sleep_hold(bool hold) {
    hold ? atomic_fetch_add(&holds, 1) : atomic_fetch_sub(&holds, 1);
}

void sleep_hold_until(int64_t when) {
    for (int64_t now = atomic_load(&held_until); now < when && !atomic_compare_exchange_weak(&held_until, &now, when);) {}
}

bool sleep_held(int64_t now) {
    return atomic_load(&holds) > 0 || now < atomic_load(&held_until);
}

unsigned sleep_sequence(void) {
    return atomic_load(&sequence);
}

sleep_wake sleep_light(uint32_t milliseconds, bool wifi) {
    enum { COUNT = sizeof sources / sizeof sources[0] };
    bool armed[COUNT];
    if (!c6_hold_transfers(5)) return SLEEP_BUSY;
    // A line low already would wake at once: what it has waits for the next time awake. No CPU interrupt: a level one
    // would fire on and on once awake. USB's pads read as GPIOs only with their input enabled, which USB does not mind. Each
    // pad keeps its awake setting asleep: its sleep one has the input off, which reads low and refuses every sleep.
    for (size_t i = 0; i < COUNT; i++)
        armed[i] = (wifi || sources[i].wake != SLEEP_WIFI) && gpio_input_enable(sources[i].pin) == ESP_OK && gpio_get_level(sources[i].pin) == 1 &&
                   gpio_intr_disable(sources[i].pin) == ESP_OK && gpio_wakeup_enable(sources[i].pin, GPIO_INTR_LOW_LEVEL) == ESP_OK &&
                   gpio_sleep_sel_dis(sources[i].pin) == ESP_OK;
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup((uint64_t)milliseconds * 1000);
    int64_t start = esp_timer_get_time();
    atomic_fetch_add(&sequence, 1);
    esp_err_t err = esp_light_sleep_start();
    atomic_fetch_add(&sequence, 1);
    // Only esp_timer counts the time asleep; lwIP's timers and the tasks' delays count ticks
    xTaskCatchUpTicks(pdMS_TO_TICKS((esp_timer_get_time() - start) / 1000));
    uint32_t causes = esp_sleep_get_wakeup_causes();
    sleep_wake wake = err != ESP_OK ? SLEEP_FAILED : SLEEP_OTHER;
    // A refusal (ESP_ERR_SLEEP_REJECT, logged as ESP_ERR_INVALID_STATE): the lines low after it, and the PMU's cause bits
    char low[32] = "";
    for (size_t i = 0; err != ESP_OK && i < COUNT; i++)
        if (armed[i] && gpio_get_level(sources[i].pin) == 0) strlcat(low, sources[i].name, sizeof low);
    for (size_t i = 0; i < COUNT; i++)
        if (armed[i]) {
            if (wake == SLEEP_OTHER && (causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) && gpio_get_level(sources[i].pin) == 0) wake = sources[i].wake;
            gpio_wakeup_disable(sources[i].pin);
            gpio_set_intr_type(sources[i].pin, GPIO_INTR_DISABLE);
        }
    // After reading D1: the read that follows takes the C6's interrupt off it
    c6_release_transfers();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    // Once for a run of the same failure
    static esp_err_t reported;
    if (err != ESP_OK && err != reported)
        ESP_LOGW(TAG, "light sleep: %s, low:%s, PMU reject cause 0x%" PRIx32, esp_err_to_name(err), low[0] ? low : " none",
                 pmu_ll_hp_get_reject_cause(&PMU));
    reported = err;
    return wake == SLEEP_OTHER && (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) ? SLEEP_TIMER : wake;
}
