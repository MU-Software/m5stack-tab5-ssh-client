// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <string.h>

#include "gesture.h"

static int failures;

// Points "x,y" while down and "up" for the lift, a drag from 24 px; what came of them: "tap x,y" and the lines scrolled
static void check_step(const char* name, int32_t step, const char* steps, const char* expected) {
    gesture g = {.slop = 24, .step = step};
    char got[256] = "", copy[256];
    snprintf(copy, sizeof copy, "%s", steps);
    for (const char* s = strtok(copy, " "); s != NULL; s = strtok(NULL, " ")) {
        int x = 0, y = 0;
        bool down = strcmp(s, "up") != 0;
        if (down) sscanf(s, "%d,%d", &x, &y);
        gesture_event e = gesture_touch(&g, down, x, y);
        size_t n = strlen(got);
        if (e.tap) snprintf(got + n, sizeof got - n, "tap %d,%d ", (int)e.x, (int)e.y);
        if (e.lines != 0) snprintf(got + n, sizeof got - n, "%+d@%d,%d ", (int)e.lines, (int)e.x, (int)e.y);
    }
    if (strcmp(got, expected) != 0) {
        printf("FAIL %s: \"%s\", expected \"%s\"\n", name, got, expected);
        failures++;
    }
}

static void check(const char* name, const char* steps, const char* expected) {
    check_step(name, 24, steps, expected);
}

int main(void) {
    check("tap", "100,200 up", "tap 100,200 ");
    check("jitter", "100,200 110,190 90,215 up", "tap 100,200 ");
    check("long press", "100,200 100,200 100,200 up", "tap 100,200 ");
    check("drag down", "100,200 100,224 100,250 100,272 up", "-1@100,200 -1@100,200 -1@100,200 ");
    check("drag up", "100,200 100,170 100,120 up", "+1@100,200 +2@100,200 ");
    check("one jump", "100,200 100,300 up", "-4@100,200 ");
    check("back", "100,200 100,250 100,200 up", "-2@100,200 +2@100,200 ");
    check("sideways", "100,200 130,205 up", "");
    check("under a step", "100,200 100,223 up", "tap 100,200 ");
    check("again", "100,200 100,260 up 50,60 up", "-2@100,200 tap 50,60 ");
    check("lift only", "up", "");
    check_step("48 px a line", 48, "100,200 100,230 100,250 100,300 up", "-1@100,200 -1@100,200 ");
    check_step("a drag under a line", 48, "100,200 100,240 up", "");
    printf("%s\n", failures ? "gesture: FAILED" : "gesture: ok");
    return failures != 0;
}
