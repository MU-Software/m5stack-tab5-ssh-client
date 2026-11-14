// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Order matters (lte.c reads ranges): the settings, the revision, the dial's checks, the network
typedef enum {
    MODEM_QUALCOMM_URC,
    MODEM_BATTERY_CHECK,
    MODEM_EDRX,
    MODEM_EDRX_NETWORK,
    MODEM_PSM,
    MODEM_MODE,
    MODEM_CATEGORY,
    MODEM_BANDS,
    MODEM_REVISION,
    MODEM_REJECTS,
    MODEM_BACKOFF,
    MODEM_REGISTRATION,
    MODEM_SYSTEM,
    MODEM_BEARER,
    MODEM_SIGNAL,
    MODEM_OPERATOR,
    MODEM_SIM,
    MODEM_ICCID,
    MODEM_NUMBER,
    MODEM_CONTEXT,
    MODEM_ADDRESS,
    MODEM_ITEMS
} modem_item;

typedef struct {
    const char *name, *read, *prefix;
    // What the firmware keeps, NULL for none: the modem gets no other AT write
    const char* write;
} modem_command;

extern const modem_command modem_commands[MODEM_ITEMS];

// values: after the prefix, empty without one; read_at: esp_timer µs, 0 for never
typedef struct {
    char values[MODEM_ITEMS][96];
    int64_t read_at[MODEM_ITEMS];
} modem_snapshot;

// Empty when line lacks prefix; an ICCID masked
void modem_value(const char* line, const char* prefix, char* value, size_t size);
// Logs and the screen never show a whole ICCID, logs never a whole number: ICCID issuer and last four, +CNUM number last four
void modem_mask(const char* line, char* out, size_t size);
// reply: the line kept so far. The last with prefix, but the first +CNUM entry with a number
bool modem_kept(const char* line, const char* prefix, const char* reply);
bool modem_backoff_clear(const char* value);
// A CGPADDR address other than ppp: the network made a new bearer
bool modem_address_moved(const char* value, const char* ppp);
// Whether modem_commands[item].write is due
bool modem_wanted(const modem_snapshot* snapshot, modem_item item);
// Returns the item whose age goes with it, MODEM_ITEMS for no such name
modem_item modem_text(const modem_snapshot* snapshot, const char* name, char* out, size_t size);
