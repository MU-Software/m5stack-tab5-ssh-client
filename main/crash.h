// SPDX-License-Identifier: GPL-3.0-or-later
// Panics and watchdog resets: what the panic handler saw, kept in RTC memory across the restart that follows
#pragma once

// At boot, before anything may panic again: logs the report of a crash that restarted this chip
void crash_start(void);
// crash: the report again, or that there was none
void crash_command(void);
