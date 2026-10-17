#pragma once
#include <stddef.h>

#include <cJSON.h>
#include <esp_err.h>

#include "state.h"

#define SD "/sdcard"

typedef enum { CONFIG_NONE, CONFIG_SD, CONFIG_NVS } config_source;

char* storage_read_file_text(const char* path);
int storage_mount(int with_usb);
cJSON* storage_get_device_config(config_source* source);
char* storage_get_ssh_config(void);
char* storage_get_known_hosts(void);
int storage_add_known_host(const char* line);
char* storage_get_private_key(const char* identity_file);
// First in the Wi-Fi list of the configuration in use (the SD card's, else NVS); a NULL password removes it
int storage_add_wifi(const char* ssid, const char* password);
// The Wi-Fi list of the configuration in use changed as config_set_wifi does; unless networks is NULL, the list as saved
// there (one allocation), and nothing saved when it cannot be made
int storage_set_wifi(const char* ssid, const wifi_network* network, int move, wifi_network** networks);
// A setting such as "log.file_kb" into the configuration in use, which takes value
int storage_set_config(const char* name, cJSON* value);
// The tmux session kept in NVS for a host alias; NULL name forgets it
esp_err_t storage_get_tmux_session(const char* alias, char* name, size_t size);
void storage_set_tmux_session(const char* alias, const char* name);
// "wifi" or "lte" off from boot, kept in NVS
bool storage_get_off(const char* what);
void storage_set_off(const char* what, bool off);
// The host alias last connected to, "" for none
void storage_get_last_host(char* alias, size_t size);
void storage_set_last_host(const char* alias);
