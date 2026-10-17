// SPDX-License-Identifier: GPL-3.0-or-later
#include "tmux.h"

#include <stdio.h>

// An F-key-like sequence that no terminal sends, as tmux's user key 99
const char tmux_end_keys[] = "\x1b[2712~";

bool tmux_command(char* out, size_t size, const char* name, bool attach, const char* old, const char* prefix, const char* keep, int age_s) {
    char start[64] = "";
    if (old != NULL)
        snprintf(start, sizeof start, "tmux rename-session -t =%s $s 2>/dev/null; ", old);
    else if (!attach)
        snprintf(start, sizeof start, "tmux new-session -d -s $s || exit $?; ");
    // -T (tmux 3.2) declares what the screen draws beyond xterm-256color's terminfo. The while loop runs in a pipeline's
    // subshell: an error there (an old tmux without a format) leaves the rest. allow-passthrough (tmux 3.3) lets kitty
    // images reach the screen, for this session's windows only. '=': an exact name, not a prefix (set-hook takes none).
    int n = snprintf(out, size,
                     "sh -c 'f=-TRGB,strikethrough,overline,usstyle,kittygraphics; tmux $f -V >/dev/null 2>&1 || f=; s=%s; t=$(date +%%s); "
                     "tmux ls -F \"#{session_name} #{session_attached} #{session_created}\" 2>/dev/null | while read n a c; do "
                     "case \"$n\" in %s*) case \"%s\" in *\" $n \"*) ;; *) [ \"$a\" = 0 ] && [ $((t - ${c:-$t})) -ge %d ] && "
                     "tmux kill-session -t \"=$n\";; esac;; esac; done; %s"
                     "tmux set -s \"user-keys[99]\" \"$(printf \"\\033[2712~\")\" 2>/dev/null && tmux bind -n User99 kill-session 2>/dev/null; "
                     "tmux set-hook -t $s after-new-window \"set -w allow-passthrough all\" 2>/dev/null; "
                     "for w in $(tmux list-windows -t =$s -F \"#{window_id}\" 2>/dev/null); do tmux set -w -t $w allow-passthrough all; done; "
                     "exec tmux $f attach-session -t =$s'",
                     name, prefix, keep, age_s, start);
    return n > 0 && (size_t)n < size;
}
