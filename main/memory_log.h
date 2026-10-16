#pragma once

// Logs internal RAM once a minute, and failed allocations when they happen
void memory_log_start(void);
void memory_log(void);
