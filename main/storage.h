#pragma once
#include <stddef.h>

#include <cJSON.h>
#include <esp_err.h>

#include "state.h"

#define SD "/sdcard"

char* storage_read_file_text(const char* path);
int storage_mount(int with_usb);
// The SD card's .config/tab5-ssh/config.json, an empty object for want of it
cJSON* storage_get_device_config(config_state* state);
char* storage_get_ssh_config(void);
char* storage_get_known_hosts(void);
int storage_add_known_host(const char* line);
char* storage_get_private_key(const char* identity_file);
// The changes below save to the SD card, making the file when there is none, never over one that is not JSON.
// First in the configuration's Wi-Fi list; a NULL password removes it
int storage_add_wifi(const char* ssid, const char* password);
// The configuration's Wi-Fi list changed as config_set_wifi does; unless networks is NULL, the list as saved
// there (one allocation), and nothing saved when it cannot be made
int storage_set_wifi(const char* ssid, const wifi_network* network, int move, wifi_network** networks);
// Replaces the configuration, which takes config, also one that is not JSON
int storage_set_device_config(cJSON* config);
// A setting such as "log.file_kb" into the configuration, which takes value
int storage_set_config(const char* name, cJSON* value);
// The session records, in .local/state/tab5-ssh/sessions.json on the SD card: how many, none without it
size_t storage_get_sessions(session_record* records, size_t capacity);
int storage_set_sessions(const session_record* records, size_t count);
