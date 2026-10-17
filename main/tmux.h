// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>

// The exit status of tmux_command when tmux session `name` is gone from the host
#define TMUX_GONE 3

// The command of an SSH session attached to tmux session `name`, which it never starts. Before that, the host's sessions
// named `prefix`... that are not in `keep` (" a b ", spaces around each), have no client and are older than age_s seconds
// are killed. With `arrows`, the wheel sends arrow keys to programs on the alternate screen that take no mouse. False when
// out is too small.
bool tmux_command(char* out, size_t size, const char* name, const char* prefix, const char* keep, int age_s, bool arrows);
// Starting tmux session `name` unattached, and killing it: exit status 0 when done, 127 without tmux
bool tmux_create_command(char* out, size_t size, const char* name);
bool tmux_kill_command(char* out, size_t size, const char* name);
