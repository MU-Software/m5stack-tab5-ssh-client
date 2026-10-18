// SPDX-License-Identifier: GPL-3.0-or-later
#include "tmux.h"

#include <stdio.h>

bool tmux_command(char* out, size_t size, const char* name, const char* prefix, const char* keep, int age_s, bool arrows) {
    // -T (tmux 3.2) declares what the screen draws beyond xterm-256color's terminfo. The while loop runs in a pipeline's
    // subshell: an error there (an old tmux without a format) leaves the rest. allow-passthrough (tmux 3.3) lets kitty
    // images reach the screen, for this session's windows only; mouse (tmux 2.1) lets touches click and scroll in it.
    // '=': an exact name, not a prefix (set-hook takes none; set takes a pane, hence '=name:'). The wheel bindings are the
    // server's, tmux 3.2's but for @tab5_arrows: sessions with it send arrow keys to programs on the alternate screen that
    // take no mouse, as Ghostty does. Not exec'd: a session ended (exit, killed, renamed) is told from a detach.
    int n = snprintf(out, size,
                     "sh -c 'f=-TRGB,strikethrough,overline,usstyle,kittygraphics; tmux $f -V >/dev/null 2>&1 || f=; s=%s; t=$(date +%%s); "
                     "tmux ls -F \"#{session_name} #{session_attached} #{session_created}\" 2>/dev/null | while read n a c; do "
                     "case \"$n\" in %s*) case \"%s\" in *\" $n \"*) ;; *) [ \"$a\" = 0 ] && [ $((t - ${c:-$t})) -ge %d ] && "
                     "tmux kill-session -t \"=$n\";; esac;; esac; done; "
                     "tmux has-session -t =$s 2>/dev/null || exit $(($? == 127 ? 127 : %d)); "
                     "tmux set-hook -t $s after-new-window \"set -w allow-passthrough all\" 2>/dev/null; "
                     "tmux set -t =$s: mouse on 2>/dev/null; tmux set -t =$s: @tab5_arrows %d; "
                     "w=\"#{?#{||:#{pane_in_mode},#{mouse_any_flag}},0,#{&&:#{alternate_on},#{@tab5_arrows}}}\"; "
                     "[ -n \"$f\" ] && tmux bind -n WheelUpPane if -F \"$w\" \"send -N 3 Up\" "
                     "\"if -F \\\"#{||:#{pane_in_mode},#{mouse_any_flag}}\\\" \\\"send -M\\\" \\\"copy-mode -e\\\"\" && "
                     "tmux bind -n WheelDownPane if -F \"$w\" \"send -N 3 Down\" \"send -M\"; "
                     "for w in $(tmux list-windows -t =$s -F \"#{window_id}\" 2>/dev/null); do tmux set -w -t $w allow-passthrough all; done; "
                     "tmux $f attach-session -t =$s; tmux has-session -t =$s 2>/dev/null || exit %d'",
                     name, prefix, keep, age_s, TMUX_GONE, arrows, TMUX_GONE);
    return n > 0 && (size_t)n < size;
}

bool tmux_create_command(char* out, size_t size, const char* name) {
    int n = snprintf(out, size, "tmux new-session -d -s %s", name);
    return n > 0 && (size_t)n < size;
}

bool tmux_kill_command(char* out, size_t size, const char* name) {
    int n = snprintf(out, size, "tmux kill-session -t =%s", name);
    return n > 0 && (size_t)n < size;
}
