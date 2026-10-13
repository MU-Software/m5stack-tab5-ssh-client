#pragma once
#include <stddef.h>

#include <cJSON.h>
#include <esp_err.h>

#define SD "/sdcard"

typedef enum { CONFIG_NONE, CONFIG_SD, CONFIG_NVS } config_source;

char* storage_read_file_text(const char* path);
int storage_mount(int with_usb);
cJSON* storage_get_device_config(config_source* source);
char* storage_get_ssh_config(void);
char* storage_get_known_hosts(void);
int storage_add_known_host(const char* line);
char* storage_get_private_key(const char* identity_file);
// The tmux session kept in NVS for a host alias; NULL name forgets it
esp_err_t storage_get_tmux_session(const char* alias, char* name, size_t size);
void storage_set_tmux_session(const char* alias, const char* name);
