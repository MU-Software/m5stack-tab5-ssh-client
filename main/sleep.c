// SPDX-License-Identifier: GPL-3.0-or-later
#include "sleep.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "keyboard_tab5.h"

static const char* TAG = "sleep";

// Each low while it has something: USB-C's D+ for a computer's bus reset or resume (no VBUS sensing, and the full-speed
// controller cannot wake the chip), SDIO D1 between transfers for the C6's interrupt
static const struct {
    gpio_num_t pin;
    sleep_wake wake;
} sources[] = {
    {KEYBOARD_TAB5_INTERRUPT, SLEEP_KEYBOARD},
    {GPIO_NUM_25, SLEEP_USB},
    {CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D1, SLEEP_WIFI},
};

sleep_wake sleep_light(uint32_t milliseconds, bool wifi) {
    enum { COUNT = sizeof sources / sizeof sources[0] };
    bool armed[COUNT];
    // A line low already would wake at once: what it has waits for the next time awake. No CPU interrupt: a level one
    // would fire on and on once awake. USB's pads read as GPIOs only with their input enabled, which USB does not mind.
    for (size_t i = 0; i < COUNT; i++)
        armed[i] = (wifi || sources[i].wake != SLEEP_WIFI) && gpio_input_enable(sources[i].pin) == ESP_OK && gpio_get_level(sources[i].pin) == 1 &&
                   gpio_intr_disable(sources[i].pin) == ESP_OK && gpio_wakeup_enable(sources[i].pin, GPIO_INTR_LOW_LEVEL) == ESP_OK;
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup((uint64_t)milliseconds * 1000);
    int64_t start = esp_timer_get_time();
    esp_err_t err = esp_light_sleep_start();
    // Only esp_timer counts the time asleep; lwIP's timers and the tasks' delays count ticks
    xTaskCatchUpTicks(pdMS_TO_TICKS((esp_timer_get_time() - start) / 1000));
    uint32_t causes = esp_sleep_get_wakeup_causes();
    sleep_wake wake = err != ESP_OK ? SLEEP_FAILED : SLEEP_OTHER;
    for (size_t i = 0; i < COUNT; i++)
        if (armed[i]) {
            if (wake == SLEEP_OTHER && (causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) && gpio_get_level(sources[i].pin) == 0) wake = sources[i].wake;
            gpio_wakeup_disable(sources[i].pin);
            gpio_set_intr_type(sources[i].pin, GPIO_INTR_DISABLE);
        }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    // Once for a run of the same failure
    static esp_err_t reported;
    if (err != ESP_OK && err != reported) ESP_LOGW(TAG, "light sleep: %s", esp_err_to_name(err));
    reported = err;
    return wake == SLEEP_OTHER && (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) ? SLEEP_TIMER : wake;
}
