// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

typedef enum { CONTROL_USB = 1, CONTROL_SSH = 2 } control_transport;

// "commands: …", those transport takes: its own (usb.c, remote.c) and main.c's
void control_help(control_transport transport, char* out, size_t size);
