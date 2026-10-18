// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the modem page shows, each read with one query: the settings (once a boot and on request), the revision (each
// time the modem is asked), the dial's checks, and the network (each dial)
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

// Each reply after its prefix (empty when the modem answered without one) and when it came (esp_timer µs, 0 for never)
typedef struct {
    char values[MODEM_ITEMS][96];
    int64_t read_at[MODEM_ITEMS];
} modem_snapshot;

// The reply line after prefix and spaces into value, empty when the line does not start with it; an ICCID masked
void modem_value(const char* line, const char* prefix, char* value, size_t size);
// An ICCID as its issuer and last four digits, the number of a +CNUM line or value as its last four, anything else as it
// is: logs and the screen never show a whole ICCID, logs never a whole number
void modem_mask(const char* line, char* out, size_t size);
// Whether at() keeps line over reply, the line kept so far: the last one starting with prefix, but the first +CNUM entry
// with a number
bool modem_kept(const char* line, const char* prefix, const char* reply);
// The NV back-off item's T3346 and timer 20 parts are 0
bool modem_backoff_clear(const char* value);
// A CGPADDR value that is an address other than 0.0.0.0 and ppp: the network made a new bearer
bool modem_address_moved(const char* value, const char* ppp);
// Whether modem_commands[item].write is due: a switch that reads 0, or eDRX the network grants for CAT-M
bool modem_wanted(const modem_snapshot* snapshot, modem_item item);
// What the modem page shows as `name`, and the item whose age goes with it; MODEM_ITEMS for no such name
modem_item modem_text(const modem_snapshot* snapshot, const char* name, char* out, size_t size);
