// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>

// From the attached client: tmux_command binds them to killing its session
extern const char tmux_end_keys[];

// The command of an SSH session attached to tmux session `name`: started first unless `attach`, or renamed from `old`
// when that is not NULL. Before that, the host's sessions named `prefix`... that are not in `keep` (" a b ", spaces
// around each), have no client and are older than age_s seconds are killed. False when out is too small.
bool tmux_command(char* out, size_t size, const char* name, bool attach, const char* old, const char* prefix, const char* keep, int age_s);
