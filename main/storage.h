#pragma once
#include <cJSON.h>

#define SD "/sdcard"

typedef enum { CONFIG_NONE, CONFIG_SD, CONFIG_NVS } config_source;

char* storage_read_file_text(const char* path);
int storage_mount(void);
cJSON* storage_get_device_config(config_source* source);
char* storage_get_ssh_config(void);
char* storage_get_known_hosts(void);
int storage_add_known_host(const char* line);
char* storage_get_private_key(const char* identity_file);
