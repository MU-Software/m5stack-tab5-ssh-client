// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_trouble.h"

#include <stddef.h>

net_trouble net_judge(bool connected, bool named, bool online) {
    return connected ? NET_FINE : !online ? NET_NO_INTERNET : !named ? NET_NO_NAME : NET_HOST_DOWN;
}

const char* net_trouble_text(net_trouble trouble) {
    static const char* const texts[] = {
        [NET_FINE] = NULL,
        [NET_HOST_DOWN] = "reconnecting: the host does not answer",
        [NET_NO_NAME] = "reconnecting: the host's name does not resolve",
        [NET_NO_INTERNET] = "reconnecting: no internet",
    };
    return (unsigned)trouble < sizeof texts / sizeof *texts ? texts[trouble] : NULL;
}
