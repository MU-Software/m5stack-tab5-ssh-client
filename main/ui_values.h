// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "state.h"
#include "ui.h"

// What the screens show: `state`, the Wi-Fi scan and the clock; read on the screen's task
const ui_source* ui_values(const global_state* state);
