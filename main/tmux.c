// SPDX-License-Identifier: GPL-3.0-or-later
#include "tmux.h"

#include <stdio.h>

// An F-key-like sequence that no terminal sends, as tmux's user key 99
const char tmux_end_keys[] = "\x1b[2712~";

bool tmux_command(char* out, size_t size, const char* name, bool attach, const char* old, const char* prefix, const char* keep, int age_s,
                  bool arrows) {
    char start[64] = "";
    if (old != NULL)
        snprintf(start, sizeof start, "tmux rename-session -t =%s $s 2>/dev/null; ", old);
    else if (!attach)
        snprintf(start, sizeof start, "tmux new-session -d -s $s || exit $?; ");
    // -T (tmux 3.2) declares what the screen draws beyond xterm-256color's terminfo. The while loop runs in a pipeline's
    // subshell: an error there (an old tmux without a format) leaves the rest. allow-passthrough (tmux 3.3) lets kitty
    // images reach the screen, for this session's windows only; mouse (tmux 2.1) lets touches click and scroll in it.
    // '=': an exact name, not a prefix (set-hook takes none; set takes a pane, hence '=name:'). The wheel bindings are the
    // server's, tmux 3.2's but for @tab5_arrows: sessions with it send arrow keys to programs on the alternate screen that
    // take no mouse, as Ghostty does.
    int n = snprintf(out, size,
                     "sh -c 'f=-TRGB,strikethrough,overline,usstyle,kittygraphics; tmux $f -V >/dev/null 2>&1 || f=; s=%s; t=$(date +%%s); "
                     "tmux ls -F \"#{session_name} #{session_attached} #{session_created}\" 2>/dev/null | while read n a c; do "
                     "case \"$n\" in %s*) case \"%s\" in *\" $n \"*) ;; *) [ \"$a\" = 0 ] && [ $((t - ${c:-$t})) -ge %d ] && "
                     "tmux kill-session -t \"=$n\";; esac;; esac; done; %s"
                     "tmux set -s \"user-keys[99]\" \"$(printf \"\\033[2712~\")\" 2>/dev/null && tmux bind -n User99 kill-session 2>/dev/null; "
                     "tmux set-hook -t $s after-new-window \"set -w allow-passthrough all\" 2>/dev/null; "
                     "tmux set -t =$s: mouse on 2>/dev/null; tmux set -t =$s: @tab5_arrows %d; "
                     "w=\"#{?#{||:#{pane_in_mode},#{mouse_any_flag}},0,#{&&:#{alternate_on},#{@tab5_arrows}}}\"; "
                     "[ -n \"$f\" ] && tmux bind -n WheelUpPane if -F \"$w\" \"send -N 3 Up\" "
                     "\"if -F \\\"#{||:#{pane_in_mode},#{mouse_any_flag}}\\\" \\\"send -M\\\" \\\"copy-mode -e\\\"\" && "
                     "tmux bind -n WheelDownPane if -F \"$w\" \"send -N 3 Down\" \"send -M\"; "
                     "for w in $(tmux list-windows -t =$s -F \"#{window_id}\" 2>/dev/null); do tmux set -w -t $w allow-passthrough all; done; "
                     "exec tmux $f attach-session -t =$s'",
                     name, prefix, keep, age_s, start, arrows);
    return n > 0 && (size_t)n < size;
}
