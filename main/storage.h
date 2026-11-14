#pragma once
#include <stddef.h>

#include <cJSON.h>
#include <esp_err.h>

#include "state.h"

#define SD "/sdcard"

char* storage_read_file_text(const char* path);
int storage_mount(int with_usb);
// An empty object for want of config.json
cJSON* storage_get_device_config(config_state* state);
char* storage_get_ssh_config(void);
char* storage_get_known_hosts(void);
int storage_add_known_host(const char* line);
char* storage_get_private_key(const char* identity_file);
// Below: the file made when missing, never saved over one that is not JSON.
// First in the Wi-Fi list; a NULL password removes it
int storage_add_wifi(const char* ssid, const char* password);
// As config_set_wifi; unless networks is NULL, *networks the list saved (one allocation), nothing saved when it cannot be made
int storage_set_wifi(const char* ssid, const wifi_network* network, int move, wifi_network** networks);
// Takes config; also over a file that is not JSON
int storage_set_device_config(cJSON* config);
// name as "log.file_kb"; takes value
int storage_set_config(const char* name, cJSON* value);
// How many, none without the file. One not read whole moves to sessions.json.bad; *whole while no .bad file is there
size_t storage_get_sessions(session_record* records, size_t capacity, bool* whole);
int storage_set_sessions(const session_record* records, size_t count);
