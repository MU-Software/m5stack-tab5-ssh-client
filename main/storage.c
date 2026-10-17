// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage.h"

#include <inttypes.h>
#include <limits.h>
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

#include "config.h"

static const char* TAG = "storage";
static sdmmc_card_t card;
static bool mounted;
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
    mounted = true;
    mkdir(SD "/.ssh", 0700);
    mkdir(SD "/.config", 0700);
    mkdir(SD "/.config/tab5-ssh", 0700);
    return 0;
}

#define CONFIG SD "/.config/tab5-ssh/config.json"

// The text holds Wi-Fi passwords, cleared before it is freed
cJSON* storage_get_device_config(config_state* state) {
    char* text = mounted ? storage_read_file_text(CONFIG) : NULL;
    cJSON* config = text != NULL ? cJSON_Parse(text) : NULL;
    *state = !mounted ? CONFIG_NO_CARD : text == NULL ? CONFIG_MISSING : config == NULL ? CONFIG_INVALID : CONFIG_LOADED;
    if (text != NULL) memset(text, 0, strlen(text));
    free(text);
    return config != NULL ? config : cJSON_CreateObject();
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

static esp_err_t set_string(const char* key, const char* value) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("sessions", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = value != NULL ? nvs_set_str(h, key, value) : nvs_erase_key(h, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t storage_get_session(int number, char* alias, size_t alias_size, char* tmux, size_t tmux_size) {
    nvs_handle_t h;
    char key[4], value[sizeof(((ssh_host*)0)->alias) + 32];
    size_t size = sizeof value;
    snprintf(key, sizeof key, "%d", number);
    esp_err_t err = nvs_open("sessions", NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_str(h, key, value, &size);
    nvs_close(h);
    char* line = err == ESP_OK ? strchr(value, '\n') : NULL;
    if (line == NULL) return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    *line = 0;
    snprintf(alias, alias_size, "%s", value);
    snprintf(tmux, tmux_size, "%s", line + 1);
    return ESP_OK;
}

void storage_set_session(int number, const char* alias, const char* tmux) {
    char key[4], value[sizeof(((ssh_host*)0)->alias) + 32];
    snprintf(key, sizeof key, "%d", number);
    snprintf(value, sizeof value, "%s\n%s", alias != NULL ? alias : "", tmux != NULL ? tmux : "");
    esp_err_t err = set_string(key, alias != NULL ? value : NULL);
    if (err != ESP_OK) ESP_LOGW(TAG, "session %d: %s", number, esp_err_to_name(err));
}

// An older firmware kept the last host and a tmux session per host, under the alias's CRC, in NVS "tmux"
void storage_migrate_sessions(void) {
    nvs_handle_t h;
    char last[sizeof(((ssh_host*)0)->alias)] = "", tmux[16] = "", key[9], alias[sizeof last], name[24];
    size_t size = sizeof last;
    // Read only, which creates nothing, until there is something to move
    if (nvs_open("tmux", NVS_READONLY, &h) != ESP_OK) return;
    esp_err_t found = nvs_get_str(h, "last", last, &size);
    snprintf(key, sizeof key, "%08" PRIx32, esp_rom_crc32_le(0, (const uint8_t*)last, strlen(last)));
    size = sizeof tmux;
    if (found != ESP_OK || nvs_get_str(h, key, tmux, &size) != ESP_OK) tmux[0] = 0;
    nvs_close(h);
    if (found != ESP_OK) return;
    storage_set_last_host(last);
    if (tmux[0] != 0 && storage_get_session(1, alias, sizeof alias, name, sizeof name) != ESP_OK) storage_set_session(1, last, tmux);
    ESP_LOGI(TAG, "moved the last host %s%s%s", last, tmux[0] != 0 ? " and its tmux session " : "", tmux);
    if (nvs_open("tmux", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_erase_all(h) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

void storage_get_last_host(char* alias, size_t size) {
    nvs_handle_t h;
    alias[0] = 0;
    if (nvs_open("sessions", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_str(h, "last", alias, &size) != ESP_OK) alias[0] = 0;
    nvs_close(h);
}

void storage_set_last_host(const char* alias) {
    esp_err_t err = set_string("last", alias);
    if (err != ESP_OK) ESP_LOGW(TAG, "last host: %s", esp_err_to_name(err));
}

// Indented, as people edit it; the text holds Wi-Fi passwords, cleared before it is freed
static int save_config(cJSON* config) {
    char* text = cJSON_Print(config);
    cJSON_Delete(config);
    FILE* f = text != NULL ? fopen(CONFIG, "w") : NULL;
    int err = f == NULL || fputs(text, f) < 0 || fputc('\n', f) < 0;
    if (f != NULL && fclose(f) != 0) err = 1;
    if (text != NULL) memset(text, 0, strlen(text));
    free(text);
    return err ? -1 : 0;
}

// Changed in place, NULL when the file is there but not JSON
static cJSON* changed_config(void) {
    config_state state;
    cJSON* config = storage_get_device_config(&state);
    if (state != CONFIG_INVALID) return config;
    ESP_LOGW(TAG, CONFIG " is not JSON: not changed");
    cJSON_Delete(config);
    return NULL;
}

int storage_set_wifi(const char* ssid, const wifi_network* network, int move, wifi_network** networks) {
    if (networks != NULL) *networks = NULL;
    xSemaphoreTake(config_lock, portMAX_DELAY);
    cJSON* config = changed_config();
    int err = config != NULL ? config_set_wifi(config, ssid, network, move) : -1;
    if (err == 0 && networks != NULL) err = config_wifi_networks(config, networks);
    if (err == 0)
        err = save_config(config);
    else
        cJSON_Delete(config);
    xSemaphoreGive(config_lock);
    if (err != 0 && networks != NULL) {
        free(*networks);
        *networks = NULL;
    }
    return err;
}

int storage_add_wifi(const char* ssid, const char* password) {
    wifi_network network = {0};
    snprintf(network.ssid, sizeof network.ssid, "%s", ssid);
    snprintf(network.password, sizeof network.password, "%s", password != NULL ? password : "");
    int err = storage_set_wifi(ssid, password != NULL ? &network : NULL, INT_MIN, NULL);
    memset(&network, 0, sizeof network);
    return err;
}

int storage_set_device_config(cJSON* config) {
    xSemaphoreTake(config_lock, portMAX_DELAY);
    int err = save_config(config);
    xSemaphoreGive(config_lock);
    return err;
}

int storage_set_config(const char* name, cJSON* value) {
    char key[32];
    int err = -1;
    xSemaphoreTake(config_lock, portMAX_DELAY);
    cJSON *config = changed_config(), *parent = config;
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
        err = save_config(config);
    }
    xSemaphoreGive(config_lock);
    return err;
}
