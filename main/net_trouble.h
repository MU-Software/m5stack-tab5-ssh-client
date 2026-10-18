// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>

// Why the SSH host could not be reached over a path
typedef enum { NET_FINE, NET_HOST_DOWN, NET_NO_NAME, NET_NO_INTERNET } net_trouble;

// connected to the host; its name resolved, with the public DNS servers if need be; the path reached the internet
net_trouble net_judge(bool connected, bool named, bool online);
// What a session waiting to reconnect shows, NULL for NET_FINE
const char* net_trouble_text(net_trouble trouble);
