// SPDX-License-Identifier: GPL-3.0-or-later
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include <cJSON.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs_flash.h>
#include <wolfssl/wolfcrypt/memory.h>

#include "clock.h"
#include "config.h"
#include "crash.h"
#include "keyboard_tab5.h"
#include "keyboard_usb.h"
#include "keys.h"
#include "logs.h"
#include "memory_log.h"
#include "lte.h"
#include "network.h"
#include "power.h"
#include "remote.h"
#include "screen.h"
#include "settings.h"
#include "sleep.h"
#include "ssh.h"
#include "storage.h"
#include "terminal.h"
#include "tmux.h"
#include "update.h"
#include "usb.h"
#include "wifi.h"

static const char* TAG = "main";
static global_state state = {
    .terminal_type = "xterm-256color",
    .terminal_size = {.columns = 106, .rows = 30, .width_pixels = 1272, .height_pixels = 720},
    .log = {.enabled = true, .file_kb = 1024, .keep_mb = 32, .keep_days = 14},
    .timezone = "KST-9",
    .echo = {true, 50},
    .touch = {48, true},
    .wifi_enabled = true,
    .lte_enabled = true,
};

// Bumped by each change of the terminal's size
static atomic_uint resizes;

typedef enum { JOB_CREATE, JOB_CONNECT, JOB_DELETE } session_job;

// What a session's task asks on the screen; serial 0 for nothing
typedef struct {
    ui_dialog_kind kind;
    char title[96], text[160], yes[16], typed[128];
    unsigned serial, answered;
    bool granted;
} session_question;

// Session number n is sessions[n - 1]: a record while used, in sessions.json once saved (its tmux session made). While
// running it has a task for one job, which ends once the job is done or it is asked to end. The main loop makes its
// terminal before starting the task, and frees it once the task ended and it is off the screen. Under sessions_lock but for
// the atomics.
typedef struct {
    session_job job;
    // Its last task, and the last one that ended
    unsigned id;
    atomic_uint finished;
    terminal* terminal;
    _Atomic(session_status) status;
    // What it waits for and since when, shown while it is
    _Atomic(const char*) waiting;
    atomic_uint waiting_since;
    ssh_session connection;
    // The path and address of its connection, or what kept it from being found
    network_route route;
    network_trouble found;
    bool used, saved, running;
    // Its task waits for the main loop
    bool starting;
    // Whether the last task that ended failed
    atomic_bool failed;
    // Asked to end; to delete its tmux session and record once it has
    atomic_bool end, deleting;
    char tmux[24], alias[sizeof(((ssh_host*)0)->alias)];
    session_question question;
    // A failure to tell on the screen, NULL for none
    const char* alert;
    // Its task's: the last question was answered no
    bool declined;
} session;
static session sessions[MAX_SESSIONS];
// The one on the screen, NULL for none
static _Atomic(session*) shown;
// Under the lock: the one the main loop is to show next
static session* to_show;
static bool show_asked;
// On the screen while no session is
static terminal* blank;
// keys_read and its Hangul composition serve one task at a time; sessions_lock is recursive
static SemaphoreHandle_t input_lock, sessions_lock;
// This device's tmux sessions are named tab5-MMMMMM-RRRRRR: the MAC's last three bytes, then random
static char tmux_prefix[16];
// Sessions connected: while any is, logs may be quieter
static atomic_int connected;
// A failure of no session to tell on the screen under its title, NULL for none
static const char* lone_alert;
static char lone_title[48];
// Asked from the screen and not yet answered: host `name`'s new session, session `number` (its tmux) deleted, or Wi-Fi
// network `name` joined (open, or with a password typed) or deleted, or a new one picked from a scan or its SSID typed
typedef enum { ASKED_SESSION, ASKED_DELETE, ASKED_WIFI_ADD, ASKED_WIFI_OPEN, ASKED_WIFI_PASSWORD, ASKED_WIFI_SSID, ASKED_WIFI_DELETE } asked_for;
typedef struct {
    bool pending;
    asked_for what;
    char name[sizeof(((ssh_host*)0)->alias)], tmux[24];
    int number;
} screen_request;
static screen_request request;
// The dialog on the screen, the main loop's: what it is for, the asking session and its question's serial, the dialog's
// serial
static struct {
    enum { DIALOG_NONE, DIALOG_REQUEST, DIALOG_QUESTION, DIALOG_ALERT } what;
    int number;
    unsigned asked, serial;
} on_screen;
// Questions wait for an answer only while there is a screen to answer them on
static atomic_bool screen_up;
// Set once the records are loaded and SSH can start: commands come before
static atomic_bool sessions_loaded;

static void lock(void) {
    xSemaphoreTakeRecursive(sessions_lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGiveRecursive(sessions_lock);
}

static int number(const session* s) {
    return (int)(s - sessions) + 1;
}

static bool is_shown(const session* s) {
    return atomic_load(&shown) == s;
}

// Under the lock: whether s is on the screen or about to be
static bool showing(const session* s) {
    return show_asked ? to_show == s : is_shown(s);
}

// Under the lock
static void ask_show(session* s) {
    to_show = s, show_asked = true;
}

static const ssh_host* host_named(const char* alias) {
    const ssh_host* h = state.hosts;
    while (h != NULL && strcmp(h->alias, alias) != 0)
        h = h->next;
    return h;
}

// A failure of no session, told on the screen
static void tell(const char* title, const char* text) {
    lock();
    snprintf(lone_title, sizeof lone_title, "%.47s", title != NULL ? title : "");
    lone_alert = text;
    unlock();
}

// Logged, and with `shown_text` (Korean, NULL for none) told on the screen; n 0 for no session. 1 when it failed, else 0
static int report(int n, bool failed, const char* shown_text, const char* format, ...) {
    char text[300], prefix[24] = "sessions";
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    if (n > 0) snprintf(prefix, sizeof prefix, "session %d", n);
    if (failed)
        ESP_LOGW(TAG, "%s: %s", prefix, text);
    else
        ESP_LOGI(TAG, "%s: %s", prefix, text);
    lock();
    if (shown_text != NULL && n > 0) sessions[n - 1].alert = shown_text;
    unlock();
    if (shown_text != NULL && n == 0) tell(NULL, shown_text);
    return failed;
}

// From the keyboards or the USB terminal port, only for the shown session, which then shows the bottom of its scrollback
static size_t read_input(const session* s, void* buffer, size_t size) {
    size_t n = 0;
    if (!is_shown(s)) return 0;
    xSemaphoreTake(input_lock, portMAX_DELAY);
    if (is_shown(s) && (n = keys_read(buffer, size)) == 0) n = usb_terminal_read(buffer, size);
    xSemaphoreGive(input_lock);
    if (n > 0) terminal_scroll_to_bottom(s->terminal);
    return n;
}

// As screen_waiting, for the screen while s is shown
static void waiting(session* s, const char* what, TickType_t since) {
    if (what == NULL || what != atomic_load(&s->waiting)) atomic_store(&s->waiting_since, since);
    atomic_store(&s->waiting, what);
    if (is_shown(s)) screen_waiting(what, since);
}

// Asked on the screen once it can show there, for s's task: false for no, a cancel or the session ending. A prompt's answer
// goes to `out`, and none typed is a no.
static bool ask(session* s, ui_dialog_kind kind, const char* title, const char* text, const char* yes, char* out, size_t size) {
    static unsigned serials;
    if (!atomic_load(&screen_up)) {
        ESP_LOGW(TAG, "session %d: no screen to ask %s on", number(s), title);
        s->declined = true;
        return false;
    }
    session_status was = atomic_load(&s->status);
    if (was != SESSION_DELETING) atomic_store(&s->status, SESSION_ASKING);
    lock();
    session_question* q = &s->question;
    q->kind = kind, q->serial = ++serials, q->answered = 0;
    snprintf(q->title, sizeof q->title, "%s", title);
    snprintf(q->text, sizeof q->text, "%s", text);
    snprintf(q->yes, sizeof q->yes, "%s", yes);
    unsigned serial = q->serial;
    unlock();
    ESP_LOGI(TAG, "session %d: asks %s on the screen", number(s), title);
    bool answered = false, granted = false;
    while (!answered && !atomic_load(&s->end)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        lock();
        answered = q->answered == serial;
        if (answered) granted = q->granted;
        if (granted && out != NULL) snprintf(out, size, "%s", q->typed);
        unlock();
    }
    lock();
    wc_ForceZero(q->typed, sizeof q->typed);
    q->serial = 0;
    unlock();
    // Unless it is being deleted meanwhile
    atomic_compare_exchange_strong(&s->status, &(session_status){SESSION_ASKING}, was);
    s->declined = !granted;
    return granted;
}

int accept_new_host(const ssh_session* connection, const char* name, const char* fingerprint, const char* line) {
    session* s = NULL;
    for (size_t i = 0; i < MAX_SESSIONS; i++)
        if (&sessions[i].connection == connection) s = &sessions[i];
    if (s == NULL) return 0;
    char title[96], text[160];
    snprintf(title, sizeof title, "처음 보는 호스트 %s", name);
    snprintf(text, sizeof text, "%s\n이 키를 믿고 연결할까요?", fingerprint);
    if (!ask(s, UI_CONFIRM, title, text, "연결", NULL, 0)) return 0;
    if (storage_add_known_host(line) != 0) ESP_LOGW(TAG, "could not add %s to " SD "/.ssh/known_hosts", name);
    return 1;
}

// Unreached: no connection was made
typedef enum { SESSION_CLOSED, SESSION_REFUSED, SESSION_DROPPED, SESSION_MOVED, SESSION_ENDED, SESSION_UNREACHED } session_end;

// Typing that 1 s brings no output pings the host; no answer for another second shows as held until anything arrives.
// Mouse reports count as typing; those from before the connection are dropped.
static session_end bridge(session* s) {
    uint8_t in[256], out[1024];
    int pending = 0, off = 0;
    TickType_t typed_at = 0, pinged_at = 0;
    bool typed = false, pinged = false, held = false;
    unsigned pongs = 0, size = atomic_load(&resizes);
    terminal_read_mouse(s->terminal, out, sizeof out);
    unsigned seen_pongs = s->connection.pongs;
    for (;;) {
        if (network_moved(&s->route.use)) return SESSION_MOVED;
        if (atomic_load(&s->end)) return SESSION_ENDED;
        if (atomic_load(&resizes) != size) size = atomic_load(&resizes), ssh_resize(&state, &s->connection);
        if (pending == 0) {
            if ((pending = (int)terminal_reply(s->terminal, in, sizeof in)) == 0 &&
                ((pending = (int)read_input(s, in, sizeof in)) > 0 || (pending = (int)terminal_read_mouse(s->terminal, in, sizeof in)) > 0)) {
                screen_typed(in, (size_t)pending);
                if (!typed) typed = true, typed_at = xTaskGetTickCount();
            }
            off = 0;
        }
        int sent = pending > 0 ? ssh_write(&s->connection, in + off, (size_t)pending) : WS_WANT_WRITE;
        int got = ssh_read(&s->connection, out, sizeof out);
        if (got == WS_CHANNEL_CLOSED) return SESSION_CLOSED;
        if ((sent < 0 && sent != WS_WANT_WRITE) || (got < 0 && got != WS_WANT_READ)) return SESSION_DROPPED;
        if (sent > 0) {
            off += sent;
            pending -= sent;
        }
        TickType_t now = xTaskGetTickCount();
        if ((got > 0 || s->connection.pongs != seen_pongs) && s->route.use.path == NETWORK_LTE) seen_pongs = s->connection.pongs, lte_received();
        if (got > 0 || (pinged && s->connection.pongs != pongs)) {
            typed = pinged = false;
            if (held) waiting(s, NULL, 0), held = false;
        } else if (typed && !pinged && now - typed_at >= pdMS_TO_TICKS(1000)) {
            pongs = s->connection.pongs, pinged = true, pinged_at = now;
            ssh_ping(&s->connection);
        } else if (pinged && !held && now - pinged_at >= pdMS_TO_TICKS(1000)) {
            waiting(s, "held", typed_at);
            held = true;
        }
        if (got > 0) {
            terminal_write(s->terminal, out, (size_t)got);
            if (is_shown(s)) usb_terminal_write(out, (size_t)got);
        } else if (sent < 0)
            ssh_wait(&s->connection, 10);
    }
}

// A command's output, dropped, until it ends, for up to 30 s
static session_end drain(session* s) {
    uint8_t out[256];
    for (TickType_t start = xTaskGetTickCount(); xTaskGetTickCount() - start < pdMS_TO_TICKS(30000);) {
        int got = ssh_read(&s->connection, out, sizeof out);
        if (got == WS_CHANNEL_CLOSED) return SESSION_CLOSED;
        if (got < 0 && got != WS_WANT_READ) return SESSION_DROPPED;
        if (got == WS_WANT_READ) ssh_wait(&s->connection, 100);
    }
    return SESSION_DROPPED;
}

// command on h, attached to the terminal or run to its end; the exit status counts only for SESSION_CLOSED, and stays -1
// when it never connected
static session_end run(session* s, const ssh_host* h, const char* command, bool attach, int* exit_status) {
    *exit_status = -1;
    if ((s->found = network_find(h->hostname, &s->route)) != NETWORK_FINE) {
        ESP_LOGW(TAG, "session %d: %s: %s", number(s), h->alias, network_trouble_text(s->found));
        return SESSION_UNREACHED;
    }
    char* private_key = storage_get_private_key(h->identity_file);
    char password[sizeof(((session_question*)0)->typed)] = "";
    s->declined = false;
    if (private_key == NULL) {
        char title[sizeof(((session_question*)0)->title)];
        snprintf(title, sizeof title, "%.32s@%.60s", h->user, h->hostname);
        if (!ask(s, UI_PASSWORD, title, "비밀번호", "확인", password, sizeof password)) return SESSION_ENDED;
    }
    char* known_hosts = storage_get_known_hosts();
    s->connection = (ssh_session){.host = h,
                                  .known_hosts = known_hosts,
                                  .private_key = private_key,
                                  .password = password[0] ? password : NULL,
                                  .command = command,
                                  .address = s->route.address,
                                  .prepare = network_bind,
                                  .context = &s->route,
                                  .timeout_ms = network_timeout_ms(s->route.use.path)};
    // Its replies come only while awake; for a bounded time, as a host key question waits for a person
    sleep_hold_until(esp_timer_get_time() + 4LL * network_timeout_ms(s->route.use.path) * 1000);
    int rc = ssh_open(&state, &s->connection);
    if (private_key != NULL) wc_ForceZero(private_key, strlen(private_key));
    free(private_key);
    free(known_hosts);
    memset(password, 0, sizeof password);
    // Refused: the same keys and host key would only be refused again; anything else may pass another time
    session_end end = s->declined ? SESSION_ENDED : rc == WS_USER_AUTH_E || rc == WS_PUBKEY_REJECTED_E ? SESSION_REFUSED : SESSION_UNREACHED;
    if (rc != WS_SUCCESS) {
        ESP_LOGE(TAG, "session %d: ssh %s@%s:%u: %s", number(s), h->user, h->hostname, h->port, wolfSSH_ErrorToName(rc));
        return end;
    }
    ESP_LOGI(TAG, "session %d connected to %s%s%s", number(s), h->hostname, attach ? "" : ": ", attach ? "" : command);
    if (attach) {
        network_keepalive(s->connection.fd, s->route.use.path);
        waiting(s, NULL, 0);
        atomic_store(&s->status, SESSION_CONNECTED);
        if (atomic_fetch_add(&connected, 1) == 0 && state.log.quiet_sessions) {
            esp_log_level_set("*", ESP_LOG_WARN);
            // They answer control commands
            for (size_t i = 0; i < 5; i++)
                esp_log_level_set((const char* const[]){"lte", "main", "network", "power", "wifi"}[i], ESP_LOG_INFO);
        }
        end = bridge(s);
        if (is_shown(s)) screen_typed(NULL, 0);
        if (atomic_fetch_sub(&connected, 1) == 1) esp_log_level_set("*", ESP_LOG_INFO);
    } else {
        end = drain(s);
    }
    ssh_close(&s->connection, exit_status);
    ESP_LOGI(TAG, "session %d %s, exit status %d", number(s),
             (const char* const[]){"closed", "refused", "dropped", "moved", "ended", "not reached"}[end], *exit_status);
    return end;
}

// Under the lock
static void save_records(void) {
    session_record records[MAX_SESSIONS];
    size_t n = 0;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].used || !sessions[i].saved) continue;
        session_record* r = &records[n++];
        *r = (session_record){.number = number(&sessions[i])};
        memcpy(r->alias, sessions[i].alias, sizeof r->alias);
        memcpy(r->tmux, sessions[i].tmux, sizeof r->tmux);
    }
    if (storage_set_sessions(records, n) != 0) ESP_LOGW(TAG, "sessions not saved to " SD "/.local/state/tab5-ssh/sessions.json");
}

static void forget(session* s) {
    lock();
    s->used = s->saved = false;
    save_records();
    unlock();
}

// " a b ": the tmux sessions of every session, which a connection leaves
static void kept(char* out, size_t size) {
    size_t n = (size_t)snprintf(out, size, " ");
    lock();
    for (size_t i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used && n < size) n += (size_t)snprintf(out + n, size - n, "%s ", sessions[i].tmux);
    unlock();
}

// Until there is a path, what LTE is doing; Enter tries LTE at once, the other keys are dropped. False when the session
// is to end.
static bool wait_path(session* s) {
    static const char* const states[] = {
        [LTE_OFF] = "no network: LTE off, Enter turns it on",
        [LTE_NO_MODEM] = "no network: no LTE modem",
        [LTE_DIALING] = "no network: LTE dialing",
        [LTE_REGISTERING] = "no network: LTE not registered",
        [LTE_WAITING] = "no network: LTE waits, Enter retries",
        [LTE_UP] = "no network",
    };
    TickType_t since = xTaskGetTickCount();
    while (!network_wait_for(200)) {
        if (atomic_load(&s->end)) return false;
        char in[64];
        size_t n = read_input(s, in, sizeof in);
        if (memchr(in, '\r', n) != NULL) network_command("lte on");
        lte_view l;
        lte_view_now(&l);
        waiting(s, states[l.state], since);
    }
    return !atomic_load(&s->end);
}

// command on h over a connection of its own: its exit status, -1 when it did not run, after up to 30 s without a path
static int once(session* s, const ssh_host* h, const char* command) {
    int exit_status;
    for (int waited = 0; !network_wait_for(200); waited += 200)
        if (waited >= 30000 || atomic_load(&s->end)) return -1;
    return run(s, h, command, false, &exit_status) == SESSION_CLOSED ? exit_status : -1;
}

static bool create_tmux(session* s, const ssh_host* h) {
    char command[64];
    int status = tmux_create_command(command, sizeof command, s->tmux) ? once(s, h, command) : -1;
    if (status == 0) {
        lock();
        s->saved = true;
        save_records();
        unlock();
        return !report(number(s), false, NULL, "created as tmux session %s on %s, not connected", s->tmux, h->alias);
    }
    forget(s);
    if (s->declined) return !report(number(s), true, NULL, "not created: not answered on the screen");
    if (status == 127) return !report(number(s), true, "호스트에 tmux 없음 · 만들지 않음", "not created: no tmux on %s", h->alias);
    return !report(number(s), true, "만들지 못함", "not created: %s %s", h->alias,
                   status < 0 ? "could not be reached or refused" : "did not start the tmux session");
}

// Its record goes in any case
static bool kill_tmux(session* s, const ssh_host* h) {
    char command[64];
    int status = h != NULL && tmux_kill_command(command, sizeof command, s->tmux) ? once(s, h, command) : -1;
    forget(s);
    if (status == 0 || status == 1)
        return !report(number(s), false, NULL, "deleted, its tmux session %s %s", s->tmux, status == 0 ? "ended" : "already gone");
    return !report(number(s), true, NULL, "deleted on the Tab5 only; its tmux session %s remains on %s, which %s", s->tmux, s->alias,
                   h == NULL ? "is not in .ssh/config" : "could not be reached or refused");
}

// Attached to its tmux session, again after each drop or move: at once after a move, else after 1 s doubling to 30 s,
// from 1 s again once it connected. Disconnected when asked, when tmux detaches it, or when the host refused it or has
// no tmux; when its tmux session is gone its record goes too. False on a failure.
static bool keep_connected(session* s, const ssh_host* h) {
    char keep[MAX_SESSIONS * 24 + 2], command[1400];
    int retry_ms = 1000, exit_status;
    while (wait_path(s)) {
        kept(keep, sizeof keep);
        if (!tmux_command(command, sizeof command, s->tmux, tmux_prefix, keep, 60, state.touch.arrows)) return false;
        session_end end = run(s, h, command, true, &exit_status);
        if (end == SESSION_ENDED) break;
        if (end == SESSION_REFUSED && s->connection.host_changed)
            return !report(number(s), true, "호스트 키가 바뀜 · 끊음", "%s's host key changed: disconnected", h->alias);
        if (end == SESSION_REFUSED) return !report(number(s), true, "서버가 거부함 · 끊음", "%s refused the login: disconnected", h->alias);
        if (end == SESSION_CLOSED && exit_status == 127)
            return !report(number(s), true, "호스트에 tmux 없음 · 끊음", "no tmux on %s: disconnected", h->alias);
        if (end == SESSION_CLOSED && exit_status == TMUX_GONE) {
            forget(s);
            return !report(number(s), true, NULL, "tmux session %s is gone from %s: deleted", s->tmux, h->alias);
        }
        if (end == SESSION_CLOSED) return !report(number(s), false, NULL, "detached in tmux: disconnected");
        atomic_store(&s->status, SESSION_RECONNECTING);
        waiting(s, "reconnecting", xTaskGetTickCount());
        if (exit_status != -1) retry_ms = 1000;
        if (end == SESSION_MOVED) continue;
        // Why: the host down, its name, or no internet on its path
        network_trouble trouble =
            end == SESSION_UNREACHED && s->found != NETWORK_FINE ? s->found : network_after_drop(h, &s->route, end == SESSION_DROPPED);
        const char* why = network_trouble_text(trouble);
        if (why != NULL) waiting(s, why, xTaskGetTickCount());
        // A new path is worth trying at once
        for (unsigned waited = 0; waited < (unsigned)retry_ms && !atomic_load(&s->end) && !network_moved(&s->route.use); waited += 100)
            vTaskDelay(pdMS_TO_TICKS(100));
        retry_ms = retry_ms < 16000 ? retry_ms * 2 : 30000;
    }
    if (!atomic_load(&s->deleting)) report(number(s), false, NULL, "disconnected; its tmux session %s stays on %s", s->tmux, h->alias);
    return true;
}

// Under the lock
static void finish(session* s, bool failed) {
    atomic_store(&s->failed, failed);
    atomic_store(&s->status, SESSION_DISCONNECTED);
    atomic_store(&s->deleting, false);
    waiting(s, NULL, 0);
    s->running = false;
    atomic_store(&s->finished, s->id);
}

static void run_session(void* arg) {
    session* s = arg;
    const ssh_host* h = host_named(s->alias);
    bool failed = h == NULL || !h->user[0], deleting;
    if (failed && s->job != JOB_DELETE)
        report(number(s), true, "호스트가 .ssh/config에 없음", "no Host %s with a User in the SD card's .ssh/config", s->alias);
    if (failed && s->job == JOB_CREATE)
        forget(s);
    else if (!failed && s->job == JOB_CREATE)
        failed = !create_tmux(s, h);
    else if (!failed && s->job == JOB_CONNECT)
        failed = !keep_connected(s, h);
    lock();
    if (!(deleting = atomic_load(&s->deleting))) finish(s, failed);
    unlock();
    if (deleting) {
        // It ended the connection
        atomic_store(&s->end, false);
        failed = !kill_tmux(s, h != NULL && h->user[0] ? h : NULL);
        lock();
        finish(s, failed);
        unlock();
    }
    vTaskDeleteWithCaps(NULL);
}

// Under the lock: job for s, whose task the main loop starts
static void start_job(session* s, session_job job) {
    static unsigned started;
    s->job = job, s->id = ++started, s->running = s->starting = true, s->declined = false;
    atomic_store(&s->end, false);
    atomic_store(&s->deleting, job == JOB_DELETE);
    atomic_store(&s->status, job == JOB_DELETE ? SESSION_DELETING : SESSION_CONNECTING);
}

// Under the lock, the main loop's: the jobs asked for, each with a terminal it keeps while it is still there. Ghostty
// needs a deeper stack to make one than a command's task has.
static void start_tasks(void) {
    char name[24];
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        session* s = &sessions[i];
        if (!s->starting) continue;
        s->starting = false;
        if (s->terminal == NULL) s->terminal = terminal_new(&state.terminal_size);
        snprintf(name, sizeof name, "session %d", number(s));
        // Ghostty's measured stack is 27-33 KiB, as fast in PSRAM; such a task never writes flash
        if (s->terminal != NULL && xTaskCreatePinnedToCoreWithCaps(run_session, name, 48 * 1024, s, 5, NULL, 1, MALLOC_CAP_SPIRAM) == pdPASS)
            continue;
        if (s->job == JOB_CREATE) forget(s);
        report(number(s), true, "메모리 부족", "no memory for its %s", s->terminal == NULL ? "terminal" : "task");
        finish(s, true);
    }
}

// For a command that waits: until task `id` of s ends, asks on the screen or, with `connect`, connects or retries, for up
// to 90 s. Its exit status.
static int wait_for(session* s, unsigned id, bool connect) {
    for (int waited = 0; waited < 90000; waited += 100) {
        if (atomic_load(&s->finished) == id) return atomic_load(&s->failed);
        session_status now = atomic_load(&s->status);
        if (now == SESSION_ASKING) return 1;
        if (connect && now == SESSION_CONNECTED) return 0;
        if (connect && now == SESSION_RECONNECTING) {
            ESP_LOGW(TAG, "session %d: not connected, tries again until session disconnect %d", number(s), number(s));
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "session %d: still at it after 90 s", number(s));
    return 1;
}

// Under the lock: session n while used
static session* numbered(int n) {
    return n >= 1 && n <= MAX_SESSIONS && sessions[n - 1].used ? &sessions[n - 1] : NULL;
}

// The commands: from the screen without `wait`, from USB and SSH with it; each returns its exit status
static int session_create(const ssh_host* h, bool wait) {
    session* s = NULL;
    lock();
    for (size_t i = 0; s == NULL && i < MAX_SESSIONS; i++)
        if (!sessions[i].used && !sessions[i].running) s = &sessions[i];
    if (s == NULL) {
        unlock();
        return report(0, true, "세션은 4개까지", "no room, %d at most", MAX_SESSIONS);
    }
    for (bool taken = true; taken;) {
        snprintf(s->tmux, sizeof s->tmux, "%s%06" PRIx32, tmux_prefix, esp_random() & 0xffffff);
        taken = false;
        for (size_t i = 0; i < MAX_SESSIONS; i++)
            taken |= &sessions[i] != s && sessions[i].used && strcmp(sessions[i].tmux, s->tmux) == 0;
    }
    snprintf(s->alias, sizeof s->alias, "%s", h->alias);
    s->used = true, s->saved = false, s->alert = NULL;
    start_job(s, JOB_CREATE);
    unsigned id = s->id;
    unlock();
    ESP_LOGI(TAG, "session %d: creating tmux session %s on %s", number(s), s->tmux, h->alias);
    return wait ? wait_for(s, id, false) : 0;
}

// Connected if it is not, and with `show` shown
static int connect_session(int n, bool wait, bool show) {
    lock();
    session* s = numbered(n);
    const char* refusal = s == NULL                                                           ? "no such session"
                          : atomic_load(&s->deleting) || (s->running && s->job == JOB_DELETE) ? "being deleted"
                          : s->running && s->job == JOB_CREATE && !show                       ? "being created"
                                                                                              : NULL;
    bool started = refusal == NULL && !s->running;
    if (started) start_job(s, JOB_CONNECT);
    if (refusal == NULL && show) ask_show(s);
    unsigned id = refusal == NULL ? s->id : 0;
    bool connecting = refusal == NULL && s->job == JOB_CONNECT;
    unlock();
    if (refusal != NULL) return report(n, true, NULL, "%s", refusal);
    if (started) ESP_LOGI(TAG, "session %d: connecting", n);
    return wait && connecting ? wait_for(s, id, true) : 0;
}

static int session_open(int n, bool wait) {
    return connect_session(n, wait, true);
}

static int session_connect(int n, bool wait) {
    return connect_session(n, wait, false);
}

static int session_disconnect(int n, bool wait) {
    lock();
    session* s = numbered(n);
    if (s != NULL && showing(s)) ask_show(NULL);
    bool connected_now = s != NULL && s->running && s->job == JOB_CONNECT && !atomic_load(&s->deleting);
    if (connected_now) atomic_store(&s->end, true);
    unsigned id = s != NULL ? s->id : 0;
    unlock();
    if (s == NULL) return report(n, true, NULL, "no such session");
    if (!connected_now) return report(n, false, NULL, "not connected");
    return wait ? wait_for(s, id, false) : 0;
}

// Hidden, disconnected, and gone with its tmux session
static int session_delete(int n, bool wait) {
    lock();
    session* s = numbered(n);
    const char* refusal = s == NULL ? "no such session" : atomic_load(&s->deleting) || (s->running && s->job == JOB_DELETE) ? "being deleted" : NULL;
    if (refusal == NULL && showing(s)) ask_show(NULL);
    if (refusal == NULL && s->running) {
        atomic_store(&s->deleting, true);
        atomic_store(&s->status, SESSION_DELETING);
        atomic_store(&s->end, true);
    } else if (refusal == NULL) {
        start_job(s, JOB_DELETE);
    }
    unsigned id = refusal == NULL ? s->id : 0;
    unlock();
    if (refusal != NULL) return report(n, true, NULL, "%s", refusal);
    ESP_LOGI(TAG, "session %d: deleting", n);
    return wait ? wait_for(s, id, false) : 0;
}

static int session_list(void) {
    static const char* const names[] = {"disconnected", "connecting", "connected", "reconnecting", "asking", "deleting"};
    int listed = 0;
    lock();
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].used) continue;
        listed++;
        ESP_LOGI(TAG, "session %d: %s, tmux %s, %s%s", number(&sessions[i]), sessions[i].alias, sessions[i].tmux,
                 names[atomic_load(&sessions[i].status)], showing(&sessions[i]) ? ", shown" : "");
    }
    unlock();
    if (listed == 0) ESP_LOGI(TAG, "no sessions");
    return 0;
}

static int (*const session_actions[])(int n, bool wait) = {session_open, session_connect, session_disconnect, session_delete};
static const char* const session_verbs[] = {"open", "connect", "disconnect", "delete"};

// session list|create HOST|open N|connect N|disconnect N|delete N; -1 for none of these
static int session_command(const char* arguments) {
    char verb[12];
    int n;
    if (!atomic_load(&sessions_loaded)) return report(0, true, NULL, "not loaded yet");
    if (strcmp(arguments, "list") == 0) return session_list();
    if (strncmp(arguments, "create ", 7) == 0) {
        const ssh_host* h = host_named(arguments + 7);
        return h != NULL ? session_create(h, true) : report(0, true, NULL, "no Host %s in the SD card's .ssh/config", arguments + 7);
    }
    if (sscanf(arguments, "%11s %d", verb, &n) != 2) return -1;
    for (size_t i = 0; i < sizeof session_verbs / sizeof session_verbs[0]; i++)
        if (strcmp(verb, session_verbs[i]) == 0) return session_actions[i](n, true);
    return -1;
}

// The main loop's: the session on the screen from now on, NULL for none
static void show(session* s) {
    if (is_shown(s)) return;
    atomic_store(&shown, s);
    terminal* t = s != NULL ? s->terminal : blank;
    terminal_show(t);
    screen_show(t);
    screen_waiting(NULL, 0);
    if (s == NULL) return;
    screen_waiting(atomic_load(&s->waiting), (TickType_t)atomic_load(&s->waiting_since));
}

// Starts the jobs asked for, shows what was asked, takes an ended session off the screen, frees the terminals of ended
// ones off it, and tells the screens
static void tend_sessions(void) {
    lock();
    start_tasks();
    // Once its task made its terminal
    if (show_asked && (to_show == NULL || to_show->terminal != NULL || !to_show->running)) {
        show(to_show != NULL && to_show->terminal != NULL ? to_show : NULL);
        show_asked = false;
    }
    session* s = atomic_load(&shown);
    if (s != NULL && !s->running) show(NULL);
    state.session_count = 0;
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        s = &sessions[i];
        if (!s->running && !is_shown(s) && s->terminal != NULL) {
            terminal_free(s->terminal);
            s->terminal = NULL;
        }
        if (s->used)
            state.sessions[state.session_count++] = (session_info){.number = number(s),
                                                                   .alias = s->alias,
                                                                   .status = atomic_load(&s->status),
                                                                   .shown = is_shown(s),
                                                                   .creating = s->running && s->job == JOB_CREATE,
                                                                   .alert = s->alert != NULL};
    }
    unlock();
}

// The records of sessions.json, all disconnected
static void load_sessions(void) {
    session_record records[MAX_SESSIONS];
    size_t count = storage_get_sessions(records, MAX_SESSIONS);
    for (size_t i = 0; i < count; i++) {
        session* s = &sessions[records[i].number - 1];
        s->used = s->saved = true;
        memcpy(s->alias, records[i].alias, sizeof s->alias);
        memcpy(s->tmux, records[i].tmux, sizeof s->tmux);
        ESP_LOGI(TAG, "session %d: %s, tmux %s%s", records[i].number, s->alias, s->tmux, host_named(s->alias) != NULL ? "" : ", not in .ssh/config");
    }
    atomic_store(&sessions_loaded, true);
}

static void refused(const char* name) {
    ESP_LOGE(TAG, "the configuration's %s is invalid", name);
}

// Before the screen and the network start: the bars, the networks and the host to check them against
static void load_config(void) {
    static const char* const problems[] = {
        [CONFIG_MISSING] = "no " SD "/.config/tab5-ssh/config.json",
        [CONFIG_NO_CARD] = "no SD card",
        [CONFIG_INVALID] = SD "/.config/tab5-ssh/config.json is not JSON",
    };
    cJSON* config = storage_get_device_config(&state.config);
    if (state.config != CONFIG_LOADED)
        ESP_LOGW(TAG, "%s: the defaults", problems[state.config]);
    else if (config_wifi_networks(config, &state.wifi) != 0)
        ESP_LOGE(TAG, "the configuration's Wi-Fi list is invalid");
    else
        ESP_LOGI(TAG, "configuration from the SD card");
    settings_load(&state, config, refused);
    cJSON_Delete(config);
    clock_timezone(state.timezone);
    logs_configure(&state.log);
    char* text = storage_get_ssh_config();
    state.hosts = text != NULL ? config_ssh_hosts(text) : NULL;
    free(text);
    static const char* const told[] = {
        [CONFIG_LOADED] = "",
        [CONFIG_MISSING] = "SD 카드에 .config/tab5-ssh/config.json 없음\n기본값으로 실행 · 설정을 바꾸면 만듦",
        [CONFIG_NO_CARD] = "SD 카드 없음\n기본값으로 실행 · 바꾼 설정은 저장 안 됨",
        [CONFIG_INVALID] = "config.json이 올바른 JSON이 아님\n기본값으로 실행 · 고칠 때까지 저장 안 됨",
    };
    static char boot[192];
    bool hostless = state.hosts == NULL && state.config != CONFIG_NO_CARD;
    snprintf(boot, sizeof boot, "%s%s%s", told[state.config], hostless && state.config != CONFIG_LOADED ? "\n" : "",
             hostless ? "SD 카드의 .ssh/config에 Host 없음" : "");
    lone_alert = boot[0] != 0 ? boot : NULL;
}

// Before a join, which turns Wi-Fi on, as wifi on save does
static void keep_wifi_on(void) {
    if (state.wifi_enabled) return;
    state.wifi_enabled = true;
    if (storage_set_config("network.wifi.enabled", cJSON_CreateTrue()) != 0) ESP_LOGW(TAG, "network.wifi.enabled not saved");
}

// wifi connect SSID [PASSWORD], "SSID" when it has spaces; saved once joined
static void wifi_connect_command(const char* arguments) {
    wifi_network* network = calloc(1, sizeof *network);
    if (network == NULL) return;
    const char* end = arguments[0] == '"' ? strchr(arguments + 1, '"') : strchr(arguments, ' ');
    const char* ssid = arguments + (arguments[0] == '"');
    size_t length = end != NULL ? (size_t)(end - ssid) : strlen(ssid);
    if (length == 0 || length >= sizeof network->ssid) {
        free(network);
        return;
    }
    memcpy(network->ssid, ssid, length);
    const char* password = end == NULL ? "" : end + 1;
    while (*password == ' ')
        password++;
    strlcpy(network->password, password, sizeof network->password);
    keep_wifi_on();
    esp_err_t err = wifi_join(network);
    ESP_LOGI(TAG, "wifi connect %s: %s", network->ssid, esp_err_to_name(err));
    if (err == ESP_OK && storage_add_wifi(network->ssid, network->password) != 0) ESP_LOGW(TAG, "%s not saved", network->ssid);
    if (err != ESP_OK) {
        memset(network, 0, sizeof *network);
        free(network);
        return;
    }
    // Prepended and never freed: the network task may be walking the list
    network->next = state.wifi;
    state.wifi = network;
    wifi_list_changed();
}

// A join from the screen takes seconds: on its own task, and saved by the main loop once joined
typedef enum { JOIN_IDLE, JOIN_RUNNING, JOIN_DONE } join_step;
static struct {
    wifi_network network;
    esp_err_t error;
    _Atomic join_step step;
} join;

static void join_wifi(void* arg) {
    (void)arg;
    join.error = wifi_join(&join.network);
    atomic_store(&join.step, JOIN_DONE);
    vTaskDeleteWithCaps(NULL);
}

static void start_join(const wifi_network* network) {
    if (atomic_load(&join.step) != JOIN_IDLE) {
        tell(network->ssid, "다른 연결을 기다리는 중");
        return;
    }
    keep_wifi_on();
    join.network = *network;
    atomic_store(&join.step, JOIN_RUNNING);
    if (xTaskCreatePinnedToCoreWithCaps(join_wifi, "join", 6144, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM) == pdPASS) return;
    memset(&join, 0, sizeof join);
    tell(network->ssid, "연결 못 함");
}

// Network ssid of the configuration in use changed, and the list the network task joins from with it; the old list is
// kept, as that task may be walking it
static bool save_wifi(const char* ssid, const wifi_network* network, int move) {
    wifi_network* networks;
    bool saved = storage_set_wifi(ssid, network, move, &networks) == 0;
    if (saved) state.wifi = networks, wifi_list_changed();
    ESP_LOGI(TAG, "wifi %s %s%s",
             network == NULL ? "delete"
             : move != 0     ? "up"
                             : "save",
             network != NULL ? network->ssid : ssid, saved ? "" : ": not saved");
    return saved;
}

static void joined_wifi(void) {
    ESP_LOGI(TAG, "wifi join %s: %s", join.network.ssid, esp_err_to_name(join.error));
    const char* failed = join.error == ESP_ERR_NOT_FOUND                   ? "보이지 않음"
                         : join.error == ESP_ERR_INVALID_STATE             ? "Wi-Fi가 시작되지 않음"
                         : join.error != ESP_OK                            ? "연결 못 함"
                         : !save_wifi(join.network.ssid, &join.network, 0) ? "연결됐지만 저장 못 함"
                                                                           : NULL;
    if (failed != NULL) tell(join.network.ssid, failed);
    wc_ForceZero(&join.network, sizeof join.network);
    memset(&join, 0, sizeof join);
}

static const wifi_network* saved_at(size_t index) {
    for (const wifi_network* w = state.wifi; w != NULL; w = w->next)
        if (w->ssid[0] != 0 && index-- == 0) return w;
    return NULL;
}

static const wifi_network* saved_named(const char* ssid) {
    for (const wifi_network* w = state.wifi; ssid[0] != 0 && w != NULL; w = w->next)
        if (strcmp(w->ssid, ssid) == 0) return w;
    return NULL;
}

// From the screen, one at a time: host `name`'s new session, session n deleted, or Wi-Fi network `name`'s step
static void request_dialog(asked_for what, const char* name, int n) {
    lock();
    const session* s = what == ASKED_DELETE ? numbered(n) : NULL;
    if (!request.pending && (what != ASKED_DELETE || s != NULL)) {
        request = (screen_request){.pending = true, .what = what, .number = n};
        snprintf(request.name, sizeof request.name, "%s", name);
        snprintf(request.tmux, sizeof request.tmux, "%s", s != NULL ? s->tmux : "");
    }
    unlock();
}

// What was asked from the screen, answered yes, with what was typed or the item picked (-1 for none)
static void requested(const screen_request* r, const char* typed, int picked) {
    wifi_network network = {0};
    wifi_visible_network v;
    if (r->what == ASKED_WIFI_ADD) {
        if (picked < 0)
            request_dialog(ASKED_WIFI_SSID, "", 0);
        else if (wifi_visible(state.wifi, (size_t)picked, &v))
            request_dialog(v.open ? ASKED_WIFI_OPEN : ASKED_WIFI_PASSWORD, v.ssid, 0);
        return;
    }
    if (r->what == ASKED_SESSION || r->what == ASKED_DELETE) {
        const ssh_host* h = r->what == ASKED_SESSION ? host_named(r->name) : NULL;
        lock();
        const session* d = numbered(r->number);
        bool same = r->what == ASKED_DELETE && d != NULL && strcmp(d->tmux, r->tmux) == 0;
        unlock();
        if (h != NULL) session_create(h, false);
        if (same) session_delete(r->number, false);
        return;
    }
    snprintf(network.ssid, sizeof network.ssid, "%.32s", r->name);
    bool valid = strlen(typed) < sizeof network.password;
    if (valid) memcpy(network.password, typed, strlen(typed) + 1);
    valid = valid && config_wifi_valid(&network);
    if (r->what == ASKED_WIFI_SSID && (typed[0] == 0 || strlen(typed) >= sizeof network.ssid))
        tell("직접 입력", "SSID는 1~32자");
    else if (r->what == ASKED_WIFI_SSID)
        request_dialog(ASKED_WIFI_PASSWORD, typed, 0);
    else if (r->what == ASKED_WIFI_PASSWORD && !valid)
        tell(r->name, "비밀번호는 없거나 8~63자");
    else if (r->what == ASKED_WIFI_OPEN || r->what == ASKED_WIFI_PASSWORD)
        start_join(&network);
    else if (r->what == ASKED_WIFI_DELETE && saved_named(r->name) != NULL && !save_wifi(r->name, NULL, 0))
        tell(r->name, "삭제 못 함");
    wc_ForceZero(&network, sizeof network);
}

// Under the lock: s has a question not answered yet
static bool asking(const session* s) {
    return s->question.serial != 0 && s->question.answered != s->question.serial;
}

// A dialog's button: the main loop's, for the dialog still on the screen
static void answer(const ui_event* e) {
    char typed[sizeof(((session_question*)0)->typed)];
    bool yes = strcmp(e->name, "dialog ok") == 0, asked = false;
    int picked = strcmp(e->name, "dialog pick") == 0 ? (int)ui_dialog_chosen() : -1;
    screen_request r;
    if (on_screen.what == DIALOG_NONE || e->index != on_screen.serial) return;
    ui_dialog_answer(typed, sizeof typed);
    ui_dialog_close();
    lock();
    session* s = on_screen.what == DIALOG_QUESTION ? &sessions[on_screen.number - 1] : NULL;
    if (s != NULL && asking(s) && s->question.serial == on_screen.asked) {
        session_question* q = &s->question;
        q->answered = q->serial, q->granted = yes && (q->kind != UI_PASSWORD || typed[0] != 0);
        memcpy(q->typed, typed, sizeof typed);
    }
    if (on_screen.what == DIALOG_REQUEST) r = request, asked = yes || picked >= 0, request.pending = false;
    on_screen.what = DIALOG_NONE;
    unlock();
    if (asked) requested(&r, typed, picked);
    wc_ForceZero(typed, sizeof typed);
}

// The main loop's, after its events: once no dialog is up, a request from the screen, else the shown session's question;
// while a screen is open also other sessions' questions and the failures. A question its task stopped asking closes.
static void tend_dialogs(void) {
    session_question q = {.kind = UI_ALERT, .yes = "확인"};
    lock();
    const session* asker = on_screen.what == DIALOG_QUESTION ? &sessions[on_screen.number - 1] : NULL;
    bool ended = asker != NULL && (!asking(asker) || asker->question.serial != on_screen.asked);
    unlock();
    if (ended) {
        ui_dialog_close();
        on_screen.what = DIALOG_NONE;
    }
    if (on_screen.what != DIALOG_NONE) return;
    lock();
    session* front = atomic_load(&shown);
    bool open = ui_panel() != NULL;
    if (front != NULL && !asking(front)) front = NULL;
    for (size_t i = 0; front == NULL && open && i < MAX_SESSIONS; i++)
        if (asking(&sessions[i])) front = &sessions[i];
    static const struct {
        ui_dialog_kind kind;
        const char *text, *yes;
    } asks[] = {
        [ASKED_SESSION] = {UI_CONFIRM, "위 호스트의 세션을 생성할까요?", "생성"},
        [ASKED_DELETE] = {UI_CONFIRM, "삭제할까요?\n서버의 tmux 세션도 함께 끝납니다", "삭제"},
        [ASKED_WIFI_ADD] = {UI_LIST, "", "직접 입력"},
        [ASKED_WIFI_OPEN] = {UI_CONFIRM, "열린 망 · 연결되면 저장", "연결"},
        [ASKED_WIFI_PASSWORD] = {UI_PASSWORD, "비밀번호를 입력하세요.\n비밀번호가 없는 개방형 네트워크는 비운 채로 저장해주세요.", "연결"},
        [ASKED_WIFI_SSID] = {UI_PROMPT, "SSID", "다음"},
        [ASKED_WIFI_DELETE] = {UI_CONFIRM, "삭제할까요?", "삭제"},
    };
    if (request.pending) {
        on_screen.what = DIALOG_REQUEST, q.kind = asks[request.what].kind;
        snprintf(q.yes, sizeof q.yes, "%s", asks[request.what].yes);
        snprintf(q.text, sizeof q.text, "%s", asks[request.what].text);
        if (request.what == ASKED_SESSION)
            snprintf(q.text, sizeof q.text, "'%.64s'\n%s", request.name, asks[request.what].text);
        else if (request.what == ASKED_DELETE)
            snprintf(q.title, sizeof q.title, "세션 %d %.64s", request.number, sessions[request.number - 1].alias);
        else
            snprintf(q.title, sizeof q.title, "%.64s",
                     request.what == ASKED_WIFI_SSID  ? "직접 입력"
                     : request.what == ASKED_WIFI_ADD ? "사용 가능한 Wi-Fi"
                                                      : request.name);
    } else if (front != NULL) {
        on_screen.what = DIALOG_QUESTION, on_screen.number = number(front), on_screen.asked = front->question.serial;
        q = front->question;
    }
    for (size_t i = 0; on_screen.what == DIALOG_NONE && open && i < MAX_SESSIONS; i++) {
        if (sessions[i].alert == NULL) continue;
        on_screen.what = DIALOG_ALERT;
        snprintf(q.title, sizeof q.title, "세션 %d %.64s", (int)i + 1, sessions[i].alias);
        snprintf(q.text, sizeof q.text, "%s", sessions[i].alert);
        sessions[i].alert = NULL;
    }
    if (on_screen.what == DIALOG_NONE && open && lone_alert != NULL) {
        on_screen.what = DIALOG_ALERT;
        snprintf(q.title, sizeof q.title, "%s", lone_title);
        snprintf(q.text, sizeof q.text, "%s", lone_alert);
        lone_alert = NULL;
    }
    unlock();
    if (on_screen.what == DIALOG_NONE) return;
    on_screen.serial = q.kind == UI_LIST ? ui_dialog_list(q.title, "visible", "  {ssid:34}{signal:10}{lock}", "wifi.scan", q.yes)
                                         : ui_dialog(q.kind, q.title[0] != 0 ? q.title : NULL, q.text, q.yes);
    if (ui_dialog_shown() != 0) return;
    ESP_LOGE(TAG, "no dialog for %s %s", q.title, q.text);
    answer(&(ui_event){.kind = UI_ACTION, .name = "dialog cancel", .index = on_screen.serial});
}

// The network screen's Wi-Fi: a saved network joined, moved up, joined with a new password or deleted, and a new one
// picked from a scan or typed; each saved once joined
static bool wifi_screen(const ui_event* e) {
    const char* action = e->kind == UI_ACTION && strncmp(e->name, "wifi ", 5) == 0 ? e->name + 5 : "";
    const wifi_network* saved = saved_at(e->index);
    if (strcmp(action, "add") == 0)
        wifi_scan(), request_dialog(ASKED_WIFI_ADD, "", 0);
    else if (strcmp(action, "join") == 0 && saved != NULL)
        start_join(saved);
    else if (strcmp(action, "up") == 0 && saved != NULL && !save_wifi(saved->ssid, saved, -1))
        tell(saved->ssid, "저장 못 함");
    else if (strcmp(action, "edit") == 0 && saved != NULL)
        request_dialog(ASKED_WIFI_PASSWORD, saved->ssid, 0);
    else if (strcmp(action, "delete") == 0 && saved != NULL)
        request_dialog(ASKED_WIFI_DELETE, saved->ssid, 0);
    else
        return false;
    return true;
}

// From the screen or the set command
static void change(const setting* s, const setting_value* value) {
    char text[80];
    settings_text(s, value, text, sizeof text);
    if (!settings_set(&state, s, value)) {
        ESP_LOGW(TAG, "%s %s refused", s->name, text);
        return;
    }
    bool saved = storage_set_config(s->name, settings_json(&state, s)) == 0;
    ESP_LOGI(TAG, "%s %s%s", s->name, text, saved ? "" : ", not saved");
    if (strncmp(s->name, "log.", 4) == 0) logs_configure(&state.log);
    if (strcmp(s->name, "network.wifi.enabled") == 0) wifi_list_changed(), network_command(state.wifi_enabled ? "wifi on" : "wifi off");
    if (strcmp(s->name, "network.lte.enabled") == 0) network_command(state.lte_enabled ? "lte on" : "lte off");
    if (strcmp(s->name, "timezone") == 0) clock_timezone(state.timezone);
}

// set NAME VALUE, the value as the configuration file keeps it, text also without its quotes
static void set_command(const char* arguments) {
    const char* value = strchr(arguments, ' ');
    char name[32];
    snprintf(name, sizeof name, "%.*s", value != NULL ? (int)(value - arguments) : 0, arguments);
    const setting* s = value != NULL ? settings_find(name) : NULL;
    cJSON* item = s != NULL ? cJSON_ParseWithOpts(value + 1, NULL, true) : NULL;
    if (s != NULL && item == NULL) item = cJSON_CreateString(value + 1);
    setting_value v;
    if (item != NULL && settings_parse(s, item, &v))
        change(s, &v);
    else
        ESP_LOGW(TAG, "set %s: no such setting or not a value for it", arguments);
    cJSON_Delete(item);
}

// modem status|read|at COMMAND; all but status run on the LTE task, which logs what came
static void modem_control(const char* arguments) {
    char line[80];
    snprintf(line, sizeof line, "modem %s", arguments);
    if (strcmp(arguments, "status") == 0)
        lte_log_snapshot();
    else if (!network_command(line))
        ESP_LOGW(TAG, "modem status|read|at COMMAND");
}

// Asked by control commands, done by the main loop
static atomic_bool sleep_asked, wake_asked, profile_asked;

// -1 for no such command, else its exit status
static int command(const char* line) {
    if (strncmp(line, "session ", 8) == 0) return session_command(line + 8);
    if (strcmp(line, "sleep") == 0 || strcmp(line, "wake") == 0 || strcmp(line, "power profile") == 0) {
        atomic_store(line[0] == 'w' ? &wake_asked : line[0] == 's' ? &sleep_asked : &profile_asked, true);
        return 0;
    }
    if (strncmp(line, "modem ", 6) == 0) {
        modem_control(line + 6);
        return 0;
    }
    if (strcmp(line, "settings") == 0) {
        const setting* s;
        for (size_t i = 0; (s = settings_at(i)) != NULL; i++) {
            cJSON* value = settings_json(&state, s);
            char* text = cJSON_PrintUnformatted(value);
            ESP_LOGI(TAG, "%s %s", s->name, text != NULL ? text : "?");
            free(text);
            cJSON_Delete(value);
        }
        return 0;
    }
    if (strncmp(line, "set ", 4) == 0) {
        set_command(line + 4);
        return 0;
    }
    if (strncmp(line, "wifi connect ", 13) == 0) {
        wifi_connect_command(line + 13);
        return 0;
    }
    if (strcmp(line, "crash") == 0) {
        crash_command();
        return 0;
    }
    if (strcmp(line, "power") == 0) {
        power_report();
        return 0;
    }
    // Without save they last the boot
    if (strcmp(line, "wifi on save") == 0 || strcmp(line, "wifi off save") == 0) {
        change(settings_find("network.wifi.enabled"), &(setting_value){.number = line[6] == 'n'});
        return 0;
    }
    if (strcmp(line, "lte on save") == 0 || strcmp(line, "lte off save") == 0) {
        change(settings_find("network.lte.enabled"), &(setting_value){.number = line[5] == 'n'});
        return 0;
    }
    if (strncmp(line, "wifi forget ", 12) == 0) {
        // Emptied, not unlinked, for the same reason
        for (wifi_network* w = state.wifi; w != NULL; w = w->next)
            if (strcmp(w->ssid, line + 12) == 0) w->ssid[0] = 0;
        ESP_LOGI(TAG, "wifi forget %s: %s", line + 12, storage_add_wifi(line + 12, NULL) == 0 ? "done" : "not saved");
        wifi_list_changed();
        return 0;
    }
    return network_command(line) ? 0 : -1;
}

// Light sleep in steps, each followed by DWELL_MS awake: lwIP's timers and the sessions' keepalives run in the time
// caught up, and esp_hosted reads what the C6 queued meanwhile
#define STEP_MS  5000
#define DWELL_MS 100

static struct {
    int64_t lit_at, off_at, awake_until, asleep_us;
    unsigned wakes[SLEEP_OTHER + 1];
    bool usb_a_off;
} sleeping;
static const char* const wakes[] = {"failed", "busy", "timer", "keyboard", "USB", "Wi-Fi", "other"};

// Once asked, or without a key or a touch for the idle time
static bool idle(void) {
    int64_t input = MAX(MAX(keys_pressed_at(), screen_touched_at()), sleeping.lit_at);
    atomic_store(&wake_asked, false);
    if (!atomic_exchange(&sleep_asked, false) &&
        (state.sleep.idle_minutes == 0 || esp_timer_get_time() - input < state.sleep.idle_minutes * 60 * 1000000LL))
        return false;
    esp_err_t err = screen_sleep(true);
    ESP_LOGI(TAG, "screen off: %s", esp_err_to_name(err));
    memset(&sleeping.wakes, 0, sizeof sleeping.wakes);
    sleeping.off_at = esp_timer_get_time(), sleeping.asleep_us = 0;
    return true;
}

// With the screen off: a key or `wake` turns it on, the key going no further; while no computer uses USB-C the Tab5 sleeps
// in steps, USB-A unpowered unless kept
static bool woken(void) {
    key_event e;
    char c;
    bool pressed = false;
    keys_divert(true);
    while (keys_take(&e, &c))
        pressed = true;
    atomic_store(&sleep_asked, false);
    if (!pressed && !atomic_exchange(&wake_asked, false)) {
        bool attached = usb_attached();
        if (attached && sleeping.usb_a_off) sleeping.usb_a_off = power_usb_a(true) != ESP_OK;
        if (attached || esp_timer_get_time() < sleeping.awake_until || sleep_held(esp_timer_get_time())) return false;
        if (!state.sleep.usb_a && !sleeping.usb_a_off) sleeping.usb_a_off = power_usb_a(false) == ESP_OK;
        int64_t start = esp_timer_get_time();
        sleep_wake wake = sleep_light(STEP_MS, state.sleep.wifi_wake);
        sleeping.asleep_us += esp_timer_get_time() - start;
        sleeping.wakes[wake]++;
        ESP_LOGD(TAG, "woken: %s", wakes[wake]);
        // A computer on USB-C needs the Tab5 awake to set it up; a connection coming in over Wi-Fi needs a few round trips
        sleeping.awake_until = esp_timer_get_time() + (wake == SLEEP_USB || wake == SLEEP_WIFI ? 3000 : DWELL_MS) * 1000;
        if (wake != SLEEP_KEYBOARD) return false;
    }
    if (sleeping.usb_a_off) sleeping.usb_a_off = power_usb_a(true) != ESP_OK;
    int64_t start = esp_timer_get_time();
    esp_err_t err = screen_sleep(false);
    while (keys_take(&e, &c))
        ;
    sleeping.lit_at = esp_timer_get_time();
    ESP_LOGI(TAG, "screen on in %lld ms: %s; off %lld s, asleep %lld s, woken by the timer %u, USB %u, Wi-Fi %u, other %u, failed %u, busy %u",
             (sleeping.lit_at - start) / 1000, esp_err_to_name(err), (sleeping.lit_at - sleeping.off_at) / 1000000, sleeping.asleep_us / 1000000,
             sleeping.wakes[SLEEP_TIMER], sleeping.wakes[SLEEP_USB], sleeping.wakes[SLEEP_WIFI], sleeping.wakes[SLEEP_OTHER],
             sleeping.wakes[SLEEP_FAILED], sleeping.wakes[SLEEP_BUSY]);
    return true;
}

// The battery's draw step by step, each the INA226's average over 8.5 s before the step ends. Only on battery: USB-C power
// cuts the pack off.
static void power_profile(bool lit) {
    static const char* const steps[] = {"keyboard LEDs at 100%", "screen on", "backlight off", "panel asleep", "USB-A off", "light sleep"};
    if (!lit) screen_sleep(false), screen_draw();
    sleeping.lit_at = esp_timer_get_time();
    ESP_LOGI(TAG, "power profile: unplug USB-C within a minute");
    for (int i = 0; usb_attached(); i++) {
        if (i == 600) {
            ESP_LOGW(TAG, "power profile: USB-C still in use");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    power_average(true);
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        if (i <= 1) keyboard_tab5_lights(i == 0 ? 100 : 0);
        if (i == 2) screen_backlight(false);
        if (i == 3) screen_sleep(true);
        if (i == 4) power_usb_a(false);
        // A sleep is refused while a wake source is pending
        sleep_wake wake = SLEEP_FAILED;
        for (int tries = 0; i == 5 && wake <= SLEEP_BUSY && tries < 5; tries++)
            if ((wake = sleep_light(40000, false)) <= SLEEP_BUSY) vTaskDelay(pdMS_TO_TICKS(200));
        if (i == 5)
            ESP_LOGI(TAG, "power profile: woken by %s", wakes[wake]);
        else
            vTaskDelay(pdMS_TO_TICKS(18000));
        ESP_LOGI(TAG, "power profile: %s", steps[i]);
        power_report();
    }
    power_average(false);
    power_usb_a(true);
    screen_sleep(false);
    screen_draw();
    sleeping.lit_at = esp_timer_get_time();
    ESP_LOGI(TAG, "power profile: done");
}

// From the screen's task, whose stack in internal RAM lets it write NVS
static void apply(const ui_event* e) {
    char verb[12];
    int n;
    if (e->kind == UI_ACTION && strncmp(e->name, "dialog ", 7) == 0) {
        answer(e);
        return;
    }
    if (e->kind == UI_ACTION && (strcmp(e->name, "wifi toggle") == 0 || strcmp(e->name, "lte toggle") == 0)) {
        // A second press soon after is taken for a slip: turning the modem off and on again risks a back-off
        static int64_t toggled[2] = {INT64_MIN / 2, INT64_MIN / 2};
        bool wifi = e->name[0] == 'w', on = wifi ? state.wifi_enabled : state.lte_enabled;
        if (esp_timer_get_time() - toggled[wifi] < 3000000) return;
        toggled[wifi] = esp_timer_get_time();
        command(wifi ? (on ? "wifi off save" : "wifi on save") : on ? "lte off save" : "lte on save");
        return;
    }
    if (wifi_screen(e) || (e->kind == UI_SET && e->setting == NULL)) return;
    if (e->kind == UI_ACTION && strcmp(e->name, "session create") == 0) {
        const ssh_host* h = state.hosts;
        for (size_t i = 0; h != NULL && i < e->index; i++)
            h = h->next;
        if (h != NULL) request_dialog(ASKED_SESSION, h->alias, 0);
        return;
    }
    // A session's row
    if (e->kind == UI_ACTION && sscanf(e->name, "session %11s", verb) == 1 &&
        (strcmp(verb, "open") == 0 || strcmp(verb, "toggle") == 0 || strcmp(verb, "delete") == 0)) {
        lock();
        n = e->index < state.session_count ? state.sessions[e->index].number : 0;
        const session* s = numbered(n);
        bool connect = s != NULL && atomic_load(&s->status) == SESSION_DISCONNECTED;
        unlock();
        if (strcmp(verb, "open") == 0 && session_open(n, false) == 0) screen_open(NULL);
        if (strcmp(verb, "toggle") == 0) (connect ? session_connect : session_disconnect)(n, false);
        if (strcmp(verb, "delete") == 0) request_dialog(ASKED_DELETE, "", n);
        return;
    }
    // A session's tab shows it; the shown one's does nothing
    if (e->kind == UI_ACTION && sscanf(e->name, "session %d", &n) == 1) {
        if (!(n >= 1 && n <= MAX_SESSIONS && is_shown(&sessions[n - 1])) && session_open(n, false) == 0) screen_open(NULL);
        return;
    }
    if (e->kind == UI_ACTION && strcmp(e->name, "reboot") == 0) {
        ESP_LOGI(TAG, "restart from the screen");
        vTaskDelay(pdMS_TO_TICKS(500)); // for the log writer
        esp_restart();
    }
    if (e->kind == UI_ACTION) {
        int status = command(e->name);
        ESP_LOGI(TAG, "%s: %s", e->name, status < 0 ? "unknown" : status != 0 ? "failed" : "done");
        return;
    }
    change(e->setting, &e->value);
}

// JSON trees, such as the layout the screen keeps, are hundreds of small blocks: in internal RAM they would take the room the
// C6's SDIO buffers need
static void* psram_malloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

static void ran(void* arg) {
    (void)arg;
    update_ran();
}

// Kept once it has run 10 s: an image that crashes while starting rolls back at the next restart
static void keep_image_later(void) {
    esp_timer_handle_t keep;
    if (esp_timer_create(&(esp_timer_create_args_t){.callback = ran, .name = "keep image"}, &keep) != ESP_OK ||
        esp_timer_start_once(keep, 10 * 1000000) != ESP_OK)
        update_ran();
}

void app_main(void) {
    cJSON_InitHooks(&(cJSON_Hooks){psram_malloc, free});
    // Before USB, whose commands may come at once
    input_lock = xSemaphoreCreateMutex();
    sessions_lock = xSemaphoreCreateRecursiveMutex();
    ESP_ERROR_CHECK(input_lock != NULL && sessions_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    memory_log_start();
    if (logs_start() != ESP_OK) ESP_LOGE(TAG, "no log ring");
    crash_start();
    if (power_start() != ESP_OK) ESP_LOGE(TAG, "no power for the C6, the USB-A port or charging");
    ESP_ERROR_CHECK(keys_start());
    esp_err_t keyboard = keyboard_usb_start();
    ESP_ERROR_CHECK(update_start());
    ESP_ERROR_CHECK(usb_start(command));
    if (keyboard != ESP_OK) ESP_LOGE(TAG, "no USB keyboard: %s", esp_err_to_name(keyboard));
    if (keyboard_tab5_start() != ESP_OK) ESP_LOGE(TAG, "no Tab5 keyboard");
    ESP_ERROR_CHECK(nvs_flash_init());
    if (storage_mount(usb_storage_boot()) != 0) ESP_LOGI(TAG, "no FAT SD card");
    if (usb_storage_boot()) return;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (clock_start() != ESP_OK) ESP_LOGE(TAG, "no SNTP");
    load_config();
    // Before the screen, which takes 2.7 s to start: the C6 restarts and Wi-Fi joins meanwhile. Also without a host, for the
    // control commands
    if (network_start(&state) != ESP_OK) ESP_LOGE(TAG, "no network");
    esp_err_t screen = screen_start(&state);
    if (screen == ESP_OK) screen_resized(&state.terminal_size);
    atomic_store(&screen_up, screen == ESP_OK);
    keep_image_later();
    if (remote_start(&state, command) != ESP_OK) ESP_LOGE(TAG, "no remote control");
    // Held for good: the sessions' own Init and Cleanup then never tear wolfCrypt down under one another
    if (wolfSSH_Init() != WS_SUCCESS) ESP_LOGE(TAG, "no wolfSSH");
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(tmux_prefix, sizeof tmux_prefix, "tab5-%02x%02x%02x-", mac[3], mac[4], mac[5]);
    blank = terminal_new(&state.terminal_size);
    ESP_ERROR_CHECK(blank != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    terminal_show(blank);
    screen_show(blank);
    load_sessions();
    if (screen != ESP_OK) ESP_LOGE(TAG, "no screen");
    // Whether a screen was open last time round
    bool lit = true, paged = true;
    for (;;) {
        ui_event e;
        terminal_size size;
        if (atomic_load(&join.step) == JOIN_DONE) joined_wifi();
        tend_sessions();
        memory_log();
        if (screen == ESP_OK && atomic_exchange(&profile_asked, false)) power_profile(lit), lit = true;
        if (screen == ESP_OK && !lit) lit = woken();
        // With no session shown a screen is open, the sessions screen unless another is, and what is typed goes nowhere
        if (lit && atomic_load(&shown) == NULL) {
            char dropped[64];
            if (screen == ESP_OK && !screen_panel_open()) screen_open("sessions");
            xSemaphoreTake(input_lock, portMAX_DELAY);
            while (keys_read(dropped, sizeof dropped) > 0 || usb_terminal_read(dropped, sizeof dropped) > 0)
                ;
            xSemaphoreGive(input_lock);
        }
        if (screen == ESP_OK && lit) {
            screen_draw();
            while (screen_event(&e))
                apply(&e);
            tend_dialogs();
            memset(&e, 0, sizeof e);
            // A screen opened takes the shown session off; a session shown closes it
            bool open = ui_panel() != NULL;
            lock();
            const session* on = atomic_load(&shown);
            if (open && !paged)
                ask_show(NULL);
            else if (open && on != NULL && showing(on))
                screen_open(NULL);
            unlock();
            paged = open;
            if (screen_resized(&size)) {
                state.terminal_size = size;
                lock();
                terminal_resize(blank, &size);
                for (size_t i = 0; i < MAX_SESSIONS; i++)
                    if (sessions[i].terminal != NULL) terminal_resize(sessions[i].terminal, &size);
                unlock();
                atomic_fetch_add(&resizes, 1);
            }
            lit = !idle();
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
