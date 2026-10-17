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
// The open sessions across restarts, in NVS "sessions": key "1" to "4", the session's number, holds its host alias and
// its tmux session, "" before one runs. A NULL alias forgets it.
esp_err_t storage_get_session(int number, char* alias, size_t alias_size, char* tmux, size_t tmux_size);
void storage_set_session(int number, const char* alias, const char* tmux);
// Once: the last host and its tmux session (as session 1) from NVS "tmux", which older firmware kept, then empties it
void storage_migrate_sessions(void);
// The host alias last connected to, "" for none; also in NVS "sessions"
void storage_get_last_host(char* alias, size_t size);
void storage_set_last_host(const char* alias);
