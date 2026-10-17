// SPDX-License-Identifier: GPL-3.0-or-later
// usage: ssh_test HOST PORT USER KEY_FILE UNKNOWN_KEY_FILE PASSWORD KNOWN_HOSTS CHANGED_HOSTS FINGERPRINT
#include "ssh.h"
#include "tmux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static ssh_host host;
static ssh_session session;
static const global_state state = {.terminal_type = "xterm-256color",
                                   .terminal_size = {.columns = 106, .rows = 30, .width_pixels = 1272, .height_pixels = 720}};
static char out[65536];
static size_t out_len;
static int failures;

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static long read_until(const char* want, int timeout_ms) {
    long start = now_ms();
    while (now_ms() - start < timeout_ms) {
        if (want != NULL && strstr(out, want) != NULL) return now_ms() - start;
        int n = ssh_read(&session, out + out_len, sizeof out - out_len - 1);
        if (n == WS_WANT_READ) {
            usleep(1000);
            continue;
        }
        if (n < 0) return -1;
        out_len += (size_t)n;
        out[out_len] = 0;
        if (out_len > sizeof out / 2) {
            memmove(out, out + out_len - 64, 65);
            out_len = 64;
        }
    }
    return -1;
}

static void send_all(const char* text) {
    size_t len = strlen(text);
    long start = now_ms();
    while (len > 0 && now_ms() - start < 5000) {
        int n = ssh_write(&session, text, len);
        if (n == WS_WANT_WRITE) {
            read_until(NULL, 1);
            continue;
        }
        if (n < 0) return;
        text += n;
        len -= (size_t)n;
    }
}

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

static int answer, asked;
static char seen[64], added[2048];

int accept_new_host(const ssh_session* connection, const char* name, const char* fingerprint, const char* line) {
    (void)connection;
    (void)name;
    snprintf(seen, sizeof seen, "%s", fingerprint);
    snprintf(added, sizeof added, "%s", line);
    asked++;
    return answer;
}

static char* read_file(const char* path) {
    static char text[4][4096];
    static int used;
    FILE* f = used < 4 ? fopen(path, "rb") : NULL;
    if (f == NULL) return NULL;
    size_t n = fread(text[used], 1, sizeof text[used] - 1, f);
    fclose(f);
    text[used][n] = 0;
    return text[used++];
}

static int open_with(const char* known_hosts, const char* private_key, const char* password) {
    session = (ssh_session){.host = &host, .known_hosts = known_hosts, .private_key = private_key, .password = password};
    return ssh_open(&state, &session);
}

// Closes again when the login works
static int login(const char* known_hosts, const char* private_key, const char* password) {
    int rc = open_with(known_hosts, private_key, password);
    if (rc == WS_SUCCESS) ssh_close(&session, NULL);
    return rc;
}

static const char *known_hosts, *private_key;
static ssh_session other, probe;

// On connection s of its own: the exit status, -1 when it did not end within 5 s, -2 without a connection
static int exec_on(ssh_session* s, const char* command) {
    *s = (ssh_session){.host = &host, .known_hosts = known_hosts, .private_key = private_key, .command = command};
    if (ssh_open(&state, s) != WS_SUCCESS) return -2;
    char buf[512];
    int r = WS_WANT_READ, status = -1;
    for (long start = now_ms(); r != WS_CHANNEL_CLOSED && now_ms() - start < 5000;)
        if ((r = ssh_read(s, buf, sizeof buf)) == WS_WANT_READ) usleep(1000);
    ssh_close(s, &status);
    return r == WS_CHANNEL_CLOSED ? status : -1;
}

static int exec_status(const char* command) {
    return exec_on(&other, command);
}

// Attached to tmux session `name` as the device is for 1.5 s, with the mouse on and the wheel's arrow keys, then whether
// the end keys ended it with status 0
static bool attach_and_end(const char* name, bool attach, const char* old, const char* keep, int age_s) {
    char command[1400], buf[2048], mouse[256];
    if (!tmux_command(command, sizeof command, name, attach, old, "tab5-test01-", keep, age_s, true)) return false;
    other = (ssh_session){.host = &host, .known_hosts = known_hosts, .private_key = private_key, .command = command};
    if (ssh_open(&state, &other) != WS_SUCCESS) return false;
    int r = WS_WANT_READ, status = -1;
    for (long start = now_ms(); now_ms() - start < 1500 && (r >= 0 || r == WS_WANT_READ);)
        if ((r = ssh_read(&other, buf, sizeof buf)) == WS_WANT_READ) usleep(1000);
    snprintf(mouse, sizeof mouse,
             "[ \"$(tmux show -t =%s: -v mouse)\" = on ] && [ \"$(tmux show -t =%s: -v @tab5_arrows)\" = 1 ] && "
             "tmux list-keys -T root WheelUpPane | grep -q tab5_arrows",
             name, name);
    bool held = (r >= 0 || r == WS_WANT_READ) && exec_on(&probe, mouse) == 0;
    for (long start = now_ms();
         r != WS_CHANNEL_CLOSED && ssh_write(&other, tmux_end_keys, strlen(tmux_end_keys)) == WS_WANT_WRITE && now_ms() - start < 1000;)
        usleep(1000);
    for (long start = now_ms(); r != WS_CHANNEL_CLOSED && (r >= 0 || r == WS_WANT_READ) && now_ms() - start < 5000;)
        if ((r = ssh_read(&other, buf, sizeof buf)) == WS_WANT_READ) usleep(1000);
    ssh_close(&other, &status);
    return held && r == WS_CHANNEL_CLOSED && status == 0;
}

// The device's tmux sessions against a real tmux: names tab5-test01-...
static void tmux_sessions(void) {
    char command[1400];
    if (exec_status("tmux -V") != 0) {
        check(0, "tmux on the test host");
        return;
    }
    check(exec_status("tmux new-session -d -s tab5-test01-young") == 0, "a session of this device's, just made");
    check(attach_and_end("tab5-test01-aaaaaa", false, NULL, " tab5-test01-aaaaaa ", 60), "a new tmux session runs until the end keys end it");
    check(exec_status("tmux has-session -t =tab5-test01-aaaaaa") == 1, "the ended session is gone from the host");
    check(exec_status("tmux has-session -t =tab5-test01-young") == 0, "an unused session younger than a minute stays");
    check(exec_status("tmux new-session -d -s tab5-test01-keep01 && tmux new-session -d -s other-cccccc") == 0, "sessions to leave");
    check(attach_and_end("tab5-test01-dddddd", false, NULL, " tab5-test01-dddddd tab5-test01-keep01 ", 0), "a connection that cleans up");
    check(exec_status("tmux has-session -t =tab5-test01-young") == 1, "this device's unused session killed");
    check(exec_status("tmux has-session -t =tab5-test01-keep01") == 0, "an open session's tmux session kept");
    check(exec_status("tmux has-session -t =other-cccccc") == 0, "another's tmux session untouched");
    check(exec_status("tmux new-session -d -s tab5-0ldname") == 0 &&
              attach_and_end("tab5-test01-eeeeee", true, "tab5-0ldname", " tab5-test01-eeeeee ", 60) &&
              exec_status("tmux has-session -t =tab5-0ldname") == 1,
          "a name from before the prefix renamed and attached");
    check(tmux_command(command, sizeof command, "tab5-test01-ffffff", true, NULL, "tab5-test01-", " ", 60, true) && exec_status(command) == 1,
          "attaching to a session that has gone fails");
    exec_status("tmux kill-server");
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 10) return 2;
    const char *key = read_file(argv[4]), *unknown_key = read_file(argv[5]), *known = read_file(argv[7]), *changed = read_file(argv[8]);
    if (key == NULL || unknown_key == NULL || known == NULL || changed == NULL) return 2;
    host.port = (uint16_t)atoi(argv[2]);
    snprintf(host.hostname, sizeof host.hostname, "%s", argv[1]);
    snprintf(host.user, sizeof host.user, "%s", argv[3]);

    check(login(known, unknown_key, NULL) == WS_USER_AUTH_E, "unknown key refused");
    check(login(changed, key, NULL) == WS_PUBKEY_REJECTED_E && asked == 0, "changed host key refused without asking");
    check(login(NULL, key, NULL) == WS_PUBKEY_REJECTED_E && asked == 1, "new host declined");
    check(strcmp(seen, argv[9]) == 0, "fingerprint shown as ssh-keygen -l prints it");
    answer = 1;
    check(login(NULL, key, NULL) == WS_SUCCESS && asked == 2, "new host accepted");
    check(login(added, key, NULL) == WS_SUCCESS && asked == 2, "the known_hosts line offered for it is found next time");
    check(login(known, NULL, "not-the-password") == WS_USER_AUTH_E, "wrong password refused");
    check(login(known, NULL, argv[6]) == WS_SUCCESS, "password login");
    int exit_status = 0;
    session = (ssh_session){.host = &host, .known_hosts = known, .private_key = key, .command = "tty; echo T''2 $TERM; exit 7"};
    if (ssh_open(&state, &session) == WS_SUCCESS) {
        check(read_until("T2 xterm-256color", 5000) >= 0 && strstr(out, "/dev/pts/") != NULL, "a command runs on a pty");
        read_until(NULL, 3000);
        ssh_close(&session, &exit_status);
    }
    check(exit_status == 7, "the command's exit status");
    out_len = 0;
    out[0] = 0;

    int rc = open_with(known, key, NULL);
    check(rc == WS_SUCCESS, "key login");
    if (rc != WS_SUCCESS) return 1;

    // While this one is open
    static ssh_session second;
    char got[256] = "";
    size_t got_len = 0;
    second = (ssh_session){.host = &host, .known_hosts = known, .private_key = key, .command = "echo S''2; exit 4"};
    rc = ssh_open(&state, &second);
    for (long start = now_ms(); rc == WS_SUCCESS && strstr(got, "S2") == NULL && now_ms() - start < 5000;) {
        int n = ssh_read(&second, got + got_len, sizeof got - got_len - 1);
        if (n > 0) got[got_len += (size_t)n] = 0;
        if (n == WS_WANT_READ) usleep(1000);
        if (n < 0 && n != WS_WANT_READ) break;
    }
    check(rc == WS_SUCCESS && strstr(got, "S2") != NULL, "a second session at once");
    if (rc == WS_SUCCESS) {
        long start = now_ms();
        while (ssh_read(&second, got, sizeof got) != WS_CHANNEL_CLOSED && now_ms() - start < 3000)
            usleep(1000);
        ssh_close(&second, &exit_status);
    }
    check(exit_status == 4, "its exit status");

    // The quotes keep the shell's echo of the command line from matching
    send_all("echo T''1 $TERM; stty size; stty -a; echo E''1\n");
    check(read_until("E1\r\n", 5000) >= 0, "shell answers");
    check(strstr(out, "T1 xterm-256color") != NULL, "TERM");
    check(strstr(out, "\r\n30 106\r\n") != NULL, "pty size 106x30");
    check(strstr(out, "intr = ^C") != NULL, "VINTR ^C");
    check(strstr(out, "iutf8") != NULL && strstr(out, "-iutf8") == NULL, "IUTF8");

    // sshd rekeys every 32 KiB (compose.yaml)
    send_all("seq 20000; echo R''1\n");
    check(read_until("R1\r\n", 10000) >= 0, "rekeys keep the session");

    out_len = 0;
    out[0] = 0;
    send_all("sleep 30; echo N''OT\n");
    read_until(NULL, 300);
    send_all("\x03");
    send_all("echo A''FTER\n");
    long ms = read_until("AFTER\r\n", 3000);
    check(ms >= 0 && strstr(out, "NOT\r\n") == NULL, "^C interrupts");
    printf("     ^C to prompt %ld ms\n", ms);

    send_all("exit 3\n");
    read_until(NULL, 3000);
    ssh_close(&session, &exit_status);
    check(exit_status == 3, "exit status");

    known_hosts = known, private_key = key;
    tmux_sessions();
    return failures != 0;
}
