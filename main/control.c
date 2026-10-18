// SPDX-License-Identifier: GPL-3.0-or-later
#include "control.h"

#include <stdio.h>
#include <string.h>

#define BOTH (CONTROL_USB | CONTROL_SSH)

static const struct {
    const char* syntax;
    unsigned transports;
} commands[] = {
    {"sd on|off", CONTROL_USB},
    {"restart", BOTH},
    {"update SIZE CRC32", BOTH},
    {"c6 update SIZE CRC32", BOTH},
    {"[c6 ]nvs list|get NAMESPACE KEY|dump [NAMESPACE]|set NAMESPACE KEY TYPE VALUE|erase NAMESPACE [KEY]", BOTH},
    {"[c6 ]nvs load [NAMESPACE]", CONTROL_SSH},
    {"log", CONTROL_SSH},
    {"version", CONTROL_SSH},
    {"config", CONTROL_SSH},
    {"config set", CONTROL_SSH},
    // main.c's
    {"power [profile]", BOTH},
    {"sleep|wake", BOTH},
    {"settings", BOTH},
    {"set NAME VALUE", BOTH},
    {"wifi on|off [save]|connect SSID [PASSWORD]|forget SSID|drop", BOTH},
    {"lte on|off [save]|status|probe dead on|off", BOTH},
    {"net status|check HOST PORT [wifi|lte]|dns dead wifi|lte|off", BOTH},
    {"modem status|read|at COMMAND", BOTH},
    {"session list|create HOST|open N|connect N|disconnect N|delete N", BOTH},
};

void control_help(control_transport transport, char* out, size_t size) {
    const char* separator = " ";
    int n = snprintf(out, size, "commands:");
    for (size_t i = 0; i < sizeof commands / sizeof commands[0] && n >= 0 && (size_t)n < size; i++) {
        if (!(commands[i].transports & transport)) continue;
        n += snprintf(out + n, size - (size_t)n, "%s%s", separator, commands[i].syntax);
        separator = ", ";
    }
}

// Not a Wi-Fi password, nor a value for NVS: [c6 ]nvs set up to its type
int control_loggable(const char* line) {
    size_t set = strncmp(line, "c6 nvs set ", 11) == 0 ? 10 : strncmp(line, "nvs set ", 8) == 0 ? 7 : 0;
    if (strncmp(line, "wifi connect ", 13) == 0) return 12;
    if (set == 0) return (int)strlen(line);
    const char* end = line + set;
    for (int i = 0; i < 3 && end != NULL; i++)
        end = strchr(end + 1, ' ');
    return end != NULL ? (int)(end - line) : (int)strlen(line);
}

int control_nvs(const char* line) {
    return strcmp(line, "c6 nvs") == 0 || strncmp(line, "c6 nvs ", 7) == 0 ? 6 : strcmp(line, "nvs") == 0 || strncmp(line, "nvs ", 4) == 0 ? 3 : 0;
}
