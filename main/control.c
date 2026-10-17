// SPDX-License-Identifier: GPL-3.0-or-later
#include "control.h"

#include <stdio.h>

#define BOTH (CONTROL_USB | CONTROL_SSH)

static const struct {
    const char* syntax;
    unsigned transports;
} commands[] = {
    {"sd on|off", CONTROL_USB},
    {"restart", BOTH},
    {"update SIZE CRC32", BOTH},
    {"c6 update SIZE CRC32", BOTH},
    {"log", CONTROL_SSH},
    {"version", CONTROL_SSH},
    {"config", CONTROL_SSH},
    {"config set", CONTROL_SSH},
    // main.c's
    {"power", BOTH},
    {"settings", BOTH},
    {"set NAME VALUE", BOTH},
    {"wifi on|off [save]|connect SSID [PASSWORD]|forget SSID", BOTH},
    {"lte on|off [save]|status", BOTH},
    {"net status|check HOST PORT", BOTH},
    {"modem status|read", BOTH},
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
