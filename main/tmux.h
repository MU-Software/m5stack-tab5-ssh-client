// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>

// tmux_command's exit status once session `name` is gone, before the attach or after
#define TMUX_GONE 3

// A command attaching to tmux session `name`, never starting it. First kills the host's sessions named `prefix`... not in
// `keep` (" a b ", spaces around each), with no client and older than age_s seconds (none for a negative age_s).
// `arrows`: the wheel sends arrow keys to alternate-screen programs that take no mouse.
bool tmux_command(char* out, size_t size, const char* name, const char* prefix, const char* keep, int age_s, bool arrows);
// Kills as tmux_command does, prints the names in `keep` the host has, one a line, then `prefix` and a dot for
// tmux_sync_whole. Exit status 0, 127 without tmux, 4 when tmux could not list them
bool tmux_sync_command(char* out, size_t size, const char* prefix, const char* keep, int age_s);
// tmux_sync_command's output whole, and naming session `name`
bool tmux_sync_whole(const char* output, const char* prefix);
bool tmux_sync_lists(const char* output, const char* name);
// Exit status 0 when done, 127 without tmux
bool tmux_create_command(char* out, size_t size, const char* name);
bool tmux_kill_command(char* out, size_t size, const char* name);
