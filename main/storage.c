// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <nvs.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <sdmmc_cmd.h>
#include <tinyusb_msc.h>

static const char* TAG = "storage";
static sdmmc_card_t card;

char* storage_read_file_text(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) return NULL;
    long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    char* text = n >= 0 && fseek(f, 0, SEEK_SET) == 0 ? calloc(1, (size_t)n + 1) : NULL;
    if (text != NULL && fread(text, 1, (size_t)n, f) != (size_t)n) {
        free(text);
        text = NULL;
    }
    fclose(f);
    return text;
}

// FAT only: ESP-IDF builds FatFs without exFAT
int storage_mount(int with_usb) {
    sd_pwr_ctrl_handle_t pwr;
    if (sd_pwr_ctrl_new_on_chip_ldo(&(sd_pwr_ctrl_ldo_config_t){.ldo_chan_id = 4}, &pwr) != ESP_OK) return -1;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0; // slot 1 is esp_hosted's link to the C6
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = pwr;
    sdmmc_slot_config_t slot = {
        .clk = 43, .cmd = 44, .d0 = 39, .d1 = 40, .d2 = 41, .d3 = 42, .cd = SDMMC_SLOT_NO_CD, .wp = SDMMC_SLOT_NO_WP, .width = 4};
    tinyusb_msc_storage_config_t usb = {
        .medium.card = &card,
        .fat_fs = {.base_path = SD, .config.max_files = 8, .do_not_format = true},
        .mount_point = with_usb ? TINYUSB_MSC_STORAGE_MOUNT_USB : TINYUSB_MSC_STORAGE_MOUNT_APP,
    };
    if (sdmmc_host_init() != ESP_OK || sdmmc_host_init_slot(host.slot, &slot) != ESP_OK || sdmmc_card_init(&host, &card) != ESP_OK ||
        tinyusb_msc_install_driver(&(tinyusb_msc_driver_config_t){.user_flags.auto_mount_off = 1}) != ESP_OK ||
        tinyusb_msc_new_storage_sdmmc(&usb, NULL) != ESP_OK)
        return -1;
    mkdir(SD "/.ssh", 0700);
    mkdir(SD "/.config", 0700);
    mkdir(SD "/.config/tab5-ssh", 0700);
    return 0;
}

static char* nvs_text(void) {
    nvs_handle_t h;
    size_t size = 0;
    char* text = NULL;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return NULL;
    if (nvs_get_str(h, "config", NULL, &size) == ESP_OK && (text = malloc(size)) != NULL && nvs_get_str(h, "config", text, &size) != ESP_OK) {
        free(text);
        text = NULL;
    }
    nvs_close(h);
    return text;
}

static cJSON* parse(char* text, const char* where) {
    cJSON* config = cJSON_Parse(text);
    if (text != NULL && config == NULL) ESP_LOGW(TAG, "%s is not JSON", where);
    free(text);
    return config;
}

cJSON* storage_get_device_config(config_source* source) {
    cJSON* config = parse(storage_read_file_text(SD "/.config/tab5-ssh/config.json"), SD "/.config/tab5-ssh/config.json");
    *source = CONFIG_SD;
    if (config == NULL) {
        config = parse(nvs_text(), "the configuration in NVS");
        *source = CONFIG_NVS;
    }
    if (config == NULL) {
        config = cJSON_CreateObject();
        cJSON_AddArrayToObject(config, "wifi");
        *source = CONFIG_NONE;
    }
    return config;
}

char* storage_get_ssh_config(void) {
    return storage_read_file_text(SD "/.ssh/config");
}

char* storage_get_known_hosts(void) {
    return storage_read_file_text(SD "/.ssh/known_hosts");
}

int storage_add_known_host(const char* line) {
    FILE* f = fopen(SD "/.ssh/known_hosts", "a");
    if (f == NULL) return -1;
    int err = fputs(line, f) < 0;
    return fclose(f) != 0 || err ? -1 : 0;
}

char* storage_get_private_key(const char* identity_file) {
    static const char* const defaults[] = {SD "/.ssh/id_ed25519", SD "/.ssh/id_ecdsa", SD "/.ssh/id_rsa"};
    char* text = NULL;
    if (identity_file[0]) {
        char path[256];
        const char* rest = identity_file[0] == '~' ? identity_file + 1 : identity_file;
        int n = identity_file[0] == '/' ? snprintf(path, sizeof path, "%s", identity_file)
                                        : snprintf(path, sizeof path, SD "%s%s", rest[0] == '/' ? "" : "/", rest);
        return n > 0 && (size_t)n < sizeof path ? storage_read_file_text(path) : NULL;
    }
    for (size_t i = 0; text == NULL && i < sizeof defaults / sizeof defaults[0]; i++)
        text = storage_read_file_text(defaults[i]);
    return text;
}
