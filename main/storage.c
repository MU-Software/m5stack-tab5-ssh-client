// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <sdmmc_cmd.h>
#include <tinyusb_msc.h>

static const char* TAG = "storage";
static sdmmc_card_t card;
// The configuration is read, changed and written again by the control commands and the screen
static SemaphoreHandle_t config_lock;

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
    config_lock = xSemaphoreCreateMutex();
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

// NVS keys are at most 15 characters: the alias's CRC
static void tmux_key(const char* alias, char key[9]) {
    snprintf(key, 9, "%08" PRIx32, esp_rom_crc32_le(0, (const uint8_t*)alias, strlen(alias)));
}

esp_err_t storage_get_tmux_session(const char* alias, char* name, size_t size) {
    nvs_handle_t h;
    char key[9];
    tmux_key(alias, key);
    esp_err_t err = nvs_open("tmux", NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_str(h, key, name, &size);
    nvs_close(h);
    return err;
}

void storage_set_tmux_session(const char* alias, const char* name) {
    nvs_handle_t h;
    char key[9];
    tmux_key(alias, key);
    if (nvs_open("tmux", NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = name != NULL ? nvs_set_str(h, key, name) : nvs_erase_key(h, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "tmux session for %s: %s", alias, esp_err_to_name(err));
    nvs_close(h);
}

bool storage_get_off(const char* what) {
    nvs_handle_t h;
    char key[16];
    uint8_t off = 0;
    snprintf(key, sizeof key, "%s_off", what);
    if (nvs_open("net", NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_u8(h, key, &off);
    nvs_close(h);
    return off != 0;
}

void storage_set_off(const char* what, bool off) {
    nvs_handle_t h;
    char key[16];
    snprintf(key, sizeof key, "%s_off", what);
    if (nvs_open("net", NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = off ? nvs_set_u8(h, key, 1) : nvs_erase_key(h, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "%s: %s", key, esp_err_to_name(err));
    nvs_close(h);
}

void storage_get_last_host(char* alias, size_t size) {
    nvs_handle_t h;
    alias[0] = 0;
    if (nvs_open("tmux", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_str(h, "last", alias, &size) != ESP_OK) alias[0] = 0;
    nvs_close(h);
}

void storage_set_last_host(const char* alias) {
    nvs_handle_t h;
    if (nvs_open("tmux", NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_str(h, "last", alias);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "last host: %s", esp_err_to_name(err));
    nvs_close(h);
}

// Indented on the SD card, where people edit it; the text holds Wi-Fi passwords, cleared before it is freed
static int save_config(cJSON* config, config_source source) {
    char* text = source == CONFIG_SD ? cJSON_Print(config) : cJSON_PrintUnformatted(config);
    int err = text == NULL;
    cJSON_Delete(config);
    if (text != NULL && source == CONFIG_SD) {
        FILE* f = fopen(SD "/.config/tab5-ssh/config.json", "w");
        err = f == NULL || fputs(text, f) < 0 || fputc('\n', f) < 0;
        if (f != NULL && fclose(f) != 0) err = 1;
    } else if (text != NULL) {
        nvs_handle_t h;
        err = nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK;
        if (!err) {
            err = nvs_set_str(h, "config", text) != ESP_OK || nvs_commit(h) != ESP_OK;
            nvs_close(h);
        }
    }
    if (text != NULL) memset(text, 0, strlen(text));
    free(text);
    return err ? -1 : 0;
}

int storage_add_wifi(const char* ssid, const char* password) {
    config_source source;
    xSemaphoreTake(config_lock, portMAX_DELAY);
    cJSON* config = storage_get_device_config(&source);
    cJSON* list = cJSON_GetObjectItemCaseSensitive(config, "wifi");
    if (!cJSON_IsArray(list)) {
        cJSON_DeleteItemFromObjectCaseSensitive(config, "wifi");
        list = cJSON_AddArrayToObject(config, "wifi");
    }
    for (int i = cJSON_GetArraySize(list) - 1; i >= 0; i--) {
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list, i), "ssid");
        if (cJSON_IsString(name) && strcmp(name->valuestring, ssid) == 0) cJSON_DeleteItemFromArray(list, i);
    }
    if (password != NULL) {
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "ssid", ssid);
        if (password[0]) cJSON_AddStringToObject(entry, "password", password);
        cJSON_InsertItemInArray(list, 0, entry);
    }
    int err = save_config(config, source);
    xSemaphoreGive(config_lock);
    return err;
}

int storage_set_config(const char* name, cJSON* value) {
    config_source source;
    char key[32];
    int err = -1;
    xSemaphoreTake(config_lock, portMAX_DELAY);
    cJSON *config = storage_get_device_config(&source), *parent = config;
    for (const char* dot; cJSON_IsObject(parent) && (dot = strchr(name, '.')) != NULL; name = dot + 1) {
        snprintf(key, sizeof key, "%.*s", (int)(dot - name), name);
        cJSON* child = cJSON_GetObjectItemCaseSensitive(parent, key);
        if (!cJSON_IsObject(child)) {
            cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
            child = cJSON_AddObjectToObject(parent, key);
        }
        parent = child;
    }
    if (!cJSON_IsObject(parent)) {
        cJSON_Delete(value);
        cJSON_Delete(config);
    } else {
        if (cJSON_GetObjectItemCaseSensitive(parent, name) != NULL)
            cJSON_ReplaceItemInObjectCaseSensitive(parent, name, value);
        else
            cJSON_AddItemToObject(parent, name, value);
        err = save_config(config, source);
    }
    xSemaphoreGive(config_lock);
    return err;
}
