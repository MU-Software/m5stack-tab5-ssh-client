// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>

// The exit status of tmux_command when tmux session `name` is gone from the host, before the attach or after it
#define TMUX_GONE 3

// The command of an SSH session attached to tmux session `name`, which it never starts. Before that, the host's sessions
// named `prefix`... that are not in `keep` (" a b ", spaces around each), have no client and are older than age_s seconds
// are killed (none for a negative age_s). With `arrows`, the wheel sends arrow keys to programs on the alternate screen that
// take no mouse. False when out is too small.
bool tmux_command(char* out, size_t size, const char* name, const char* prefix, const char* keep, int age_s, bool arrows);
// The host's sessions put right as tmux_command does, printing the names in `keep` it has, one a line, then `prefix` and a
// dot, which tmux_sync_whole looks for: exit status 0 then, 127 without tmux, 4 when tmux could not list them
bool tmux_sync_command(char* out, size_t size, const char* prefix, const char* keep, int age_s);
// The output of tmux_sync_command ending in its last line, and naming tmux session `name`
bool tmux_sync_whole(const char* output, const char* prefix);
bool tmux_sync_lists(const char* output, const char* name);
// Starting tmux session `name` unattached, and killing it: exit status 0 when done, 127 without tmux
bool tmux_create_command(char* out, size_t size, const char* name);
bool tmux_kill_command(char* out, size_t size, const char* name);
