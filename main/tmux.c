// SPDX-License-Identifier: GPL-3.0-or-later
#include "tmux.h"

#include <stdio.h>
#include <string.h>

// The listing TMUX_ORPHANS reads
#define TMUX_LIST "tmux ls -F \"#{session_attached} #{session_created} #{session_name}\""
// Kills each session listed on stdin named $p... that is not in $k, has no client and was made $g s or more before $t
#define TMUX_ORPHANS                                                                                                          \
    "while read -r a c n; do case $n in $p*) case $k in *\" $n \"*) ;; *) [ \"$a\" = 0 ] && [ $((t - ${c:-$t})) -ge $g ] && " \
    "tmux kill-session -t \"=$n\" </dev/null >/dev/null 2>&1;; esac;; esac; done; "

bool tmux_command(char* out, size_t size, const char* name, const char* prefix, const char* keep, int age_s, bool arrows) {
    // -T (tmux 3.2) declares what the screen draws beyond xterm-256color's terminfo. The while loop runs in a pipeline's
    // subshell: an error there (an old tmux without a format) leaves the rest. allow-passthrough (tmux 3.3) lets kitty
    // images reach the screen, for this session's windows only; mouse (tmux 2.1) lets touches click and scroll in it.
    // '=': an exact name, not a prefix (set-hook takes none; set takes a pane, hence '=name:'). The wheel bindings are the
    // server's, tmux 3.2's but for @tab5_arrows: sessions with it send arrow keys to programs on the alternate screen that
    // take no mouse, as Ghostty does. Not exec'd: a session ended (exit, killed, renamed) is told from a detach. Only the
    // attach has the terminal: a client on it reads the host's terminfo, which may lack xterm-256color.
    int n = snprintf(out, size,
                     "sh -c 'exec 3<&0 </dev/null; f=-TRGB,strikethrough,overline,usstyle,kittygraphics; tmux $f -V >/dev/null 2>&1 || f=; "
                     "s=%s; p=%s; k=\"%s\"; g=%d; t=$(date +%%s); %s"
                     "tmux has-session -t =$s 2>/dev/null || exit $(($? == 127 ? 127 : %d)); "
                     "tmux set-hook -t $s after-new-window \"set -w allow-passthrough all\" 2>/dev/null; "
                     "tmux set -t =$s: mouse on 2>/dev/null; tmux set -t =$s: @tab5_arrows %d; "
                     "w=\"#{?#{||:#{pane_in_mode},#{mouse_any_flag}},0,#{&&:#{alternate_on},#{@tab5_arrows}}}\"; "
                     "[ -n \"$f\" ] && tmux bind -n WheelUpPane if -F \"$w\" \"send -N 3 Up\" "
                     "\"if -F \\\"#{||:#{pane_in_mode},#{mouse_any_flag}}\\\" \\\"send -M\\\" \\\"copy-mode -e\\\"\" && "
                     "tmux bind -n WheelDownPane if -F \"$w\" \"send -N 3 Down\" \"send -M\"; "
                     "for w in $(tmux list-windows -t =$s -F \"#{window_id}\" 2>/dev/null); do tmux set -w -t $w allow-passthrough all; done; "
                     "tmux $f attach-session -t =$s <&3; tmux has-session -t =$s 2>/dev/null || exit %d'",
                     name, prefix, keep, age_s, age_s >= 0 ? TMUX_LIST " 2>/dev/null | " TMUX_ORPHANS : "", TMUX_GONE, arrows, TMUX_GONE);
    return n > 0 && (size_t)n < size;
}

bool tmux_sync_command(char* out, size_t size, const char* prefix, const char* keep, int age_s) {
    // tmux 3.5a says one of these two when no server runs; a server whose socket a cleaner removed still runs. Names are
    // printed in a loop of their own, as a bad creation time ends the killing loop's subshell. printf, as some echo
    // commands take a backslash in a name for an escape.
    int n = snprintf(out, size,
                     "sh -c 'p=%s; k=\"%s\"; g=%d; t=$(date +%%s); l=$(" TMUX_LIST " 2>&1 </dev/null); r=$?; [ $r = 127 ] && exit 127; "
                     "[ $r = 0 ] || case $l in \"no server running on \"*|\"error connecting to \"*\" (No such file or directory)\") "
                     "pgrep -u $(id -u) tmux >/dev/null 2>&1; [ $? = 1 ] || exit 4; l=;; *) exit 4;; esac; "
                     "printf \"%%s\\n\" \"$l\" | while read -r a c n; do case $k in *\" $n \"*) printf \"%%s\\n\" \"$n\";; esac; done; %s"
                     "echo \"$p.\"'",
                     prefix, keep, age_s, age_s >= 0 ? "printf \"%s\\n\" \"$l\" | " TMUX_ORPHANS : "");
    return n > 0 && (size_t)n < size;
}

// The next line of *text without its \r\n, false after the last
static bool next_line(const char** text, const char** line, size_t* length) {
    if (**text == 0) return false;
    size_t n = strcspn(*text, "\r\n");
    *line = *text, *length = n;
    *text += n;
    *text += strspn(*text, "\r\n");
    return true;
}

bool tmux_sync_whole(const char* output, const char* prefix) {
    const char *line, *last = NULL;
    size_t length, last_length = 0;
    while (next_line(&output, &line, &length))
        if (length > 0) last = line, last_length = length;
    size_t p = strlen(prefix);
    return last != NULL && last_length == p + 1 && memcmp(last, prefix, p) == 0 && last[p] == '.';
}

bool tmux_sync_lists(const char* output, const char* name) {
    const char* line;
    size_t length;
    while (next_line(&output, &line, &length))
        if (length == strlen(name) && memcmp(line, name, length) == 0) return true;
    return false;
}

bool tmux_create_command(char* out, size_t size, const char* name) {
    int n = snprintf(out, size, "tmux new-session -d -s %s", name);
    return n > 0 && (size_t)n < size;
}

bool tmux_kill_command(char* out, size_t size, const char* name) {
    int n = snprintf(out, size, "tmux kill-session -t =%s", name);
    return n > 0 && (size_t)n < size;
}
