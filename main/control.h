// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

typedef enum { CONTROL_USB = 1, CONTROL_SSH = 2 } control_transport;

// Longest command line, with its terminator
#define CONTROL_LINE 512

// "commands: …", those transport takes: its own (usb.c, remote.c) and main.c's
void control_help(control_transport transport, char* out, size_t size);
// How much of a command line a log may show
int control_loggable(const char* line);
// The length of its "nvs" or "c6 nvs", 0 for another command
int control_nvs(const char* line);
