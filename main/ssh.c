// SPDX-License-Identifier: GPL-3.0-or-later
#include "ssh.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wolfssl/wolfcrypt/hash.h>

#include "config.h"

#define TIMEOUT_MS 10000

#define MODE(op, v) (op), 0, 0, (v) >> 8, (v) & 0xff
// A fresh pty otherwise keeps sshd's defaults, which on Linux lack IUTF8 (Hangul backspace deletes one byte)
static const byte pty_modes[] = {
    MODE(1, 3),       // VINTR ^C
    MODE(2, 0x1c),    // VQUIT ^backslash
    MODE(3, 0x7f),    // VERASE
    MODE(4, 0x15),    // VKILL ^U
    MODE(5, 4),       // VEOF ^D
    MODE(10, 0x1a),   // VSUSP ^Z
    MODE(36, 1),      // ICRNL
    MODE(42, 1),      // IUTF8
    MODE(50, 1),      // ISIG
    MODE(51, 1),      // ICANON
    MODE(53, 1),      // ECHO
    MODE(54, 1),      // ECHOE
    MODE(55, 1),      // ECHOK
    MODE(70, 1),      // OPOST
    MODE(72, 1),      // ONLCR
    MODE(128, 38400), // ISPEED
    MODE(129, 38400), // OSPEED
};

static int io_recv(WOLFSSH* ssh, void* buf, word32 sz, void* ctx) {
    (void)ssh;
    ssize_t n = recv(*(int*)ctx, buf, sz, 0);
    if (n > 0) return (int)n;
    if (n == 0) return WS_CBIO_ERR_CONN_CLOSE;
    return errno == EAGAIN || errno == EWOULDBLOCK ? WS_CBIO_ERR_WANT_READ : WS_CBIO_ERR_GENERAL;
}

static int io_send(WOLFSSH* ssh, void* buf, word32 sz, void* ctx) {
    (void)ssh;
    ssize_t n = send(*(int*)ctx, buf, sz, 0);
    if (n >= 0) return (int)n;
    return errno == EAGAIN || errno == EWOULDBLOCK ? WS_CBIO_ERR_WANT_WRITE : WS_CBIO_ERR_GENERAL;
}

static int check_same_host_key(const byte* key, word32 size, void* ctx) {
    const global_state* state = ctx;
    byte hash[WC_SHA256_DIGEST_SIZE];
    return wc_Sha256Hash(key, size, hash) == 0 && memcmp(hash, state->session->host_key_sha256, sizeof hash) == 0 ? 0 : -1;
}

static int check_host_key(const byte* key, word32 size, void* ctx) {
    const global_state* state = ctx;
    ssh_session* s = state->session;
    const ssh_host* h = state->current_host;
    char name[264], fingerprint[64], line[2048];
    snprintf(name, sizeof name, h->port == 22 ? "%s" : "[%s]:%u", h->hostname, h->port);
    host_state found = s->known_hosts != NULL ? known_hosts_check(s->known_hosts, name, key, size) : HOST_UNKNOWN;
    if (found == HOST_CHANGED) return -1;
    if (found == HOST_UNKNOWN && (ssh_fingerprint(fingerprint, sizeof fingerprint, key, size) != 0 ||
                                  known_hosts_line(line, sizeof line, name, key, size) != 0 || !accept_new_host(state, name, fingerprint, line)))
        return -1;
    return wc_Sha256Hash(key, size, s->host_key_sha256) == 0 ? 0 : -1;
}

// wolfSSH signs with s->key after this returns
static int read_key(ssh_session* s, WS_UserAuthData_PublicKey* pk) {
    int rc = wolfSSH_ReadKey_buffer((const byte*)s->private_key, (word32)strlen(s->private_key), WOLFSSH_FORMAT_OPENSSH, &s->key, &s->key_size,
                                    &pk->publicKeyType, &pk->publicKeyTypeSz, NULL);
    if (rc != WS_SUCCESS) return rc;
    size_t public_key_size;
    if (openssh_public_key(s->key, s->key_size, &pk->publicKey, &public_key_size) != 0) return WS_PARSE_E;
    pk->publicKeySz = (word32)public_key_size;
    pk->privateKey = s->key;
    pk->privateKeySz = s->key_size;
    return WS_SUCCESS;
}

// wolfSSH calls again once the server refused what was offered; each is offered once
static int user_auth(byte type, WS_UserAuthData* d, void* ctx) {
    const global_state* state = ctx;
    ssh_session* s = state->session;
    if (type == WOLFSSH_USERAUTH_PASSWORD && s->password != NULL) {
        d->sf.password.password = (const byte*)s->password;
        d->sf.password.passwordSz = (word32)strlen(s->password);
        s->password = NULL;
        return WOLFSSH_USERAUTH_SUCCESS;
    }
    if (type == WOLFSSH_USERAUTH_PUBLICKEY && s->private_key != NULL) {
        s->error = read_key(s, &d->sf.publicKey);
        s->private_key = NULL;
        return s->error == WS_SUCCESS ? WOLFSSH_USERAUTH_SUCCESS : WOLFSSH_USERAUTH_FAILURE;
    }
    if (s->error == WS_SUCCESS && s->password == NULL && s->private_key == NULL) s->error = WS_USER_AUTH_E;
    return WOLFSSH_USERAUTH_FAILURE;
}

static int dial(const char* host, uint16_t port) {
    char service[6];
    snprintf(service, sizeof service, "%u", port);
    struct addrinfo* ai;
    if (getaddrinfo(host, service, &(struct addrinfo){.ai_socktype = SOCK_STREAM}, &ai) != 0) return -1;
    int fd = socket(ai->ai_family, SOCK_STREAM, 0);
    if (fd >= 0 && (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0 || fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != 0)) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd >= 0) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
    return fd;
}

static void forget_key(ssh_session* s) {
    if (s->key != NULL) wc_ForceZero(s->key, s->key_size);
    WFREE(s->key, NULL, 0);
    s->key = NULL;
}

static int code(const ssh_session* s, int r) {
    return r == WS_FATAL_ERROR ? wolfSSH_get_error(s->ssh) : r;
}

int ssh_open(const global_state* state) {
    ssh_session* s = state->session;
    const ssh_host* h = state->current_host;
    const terminal_size* size = &state->terminal_size;
    *s = (ssh_session){.known_hosts = s->known_hosts, .private_key = s->private_key, .password = s->password, .command = s->command, .fd = -1};
    if (wolfSSH_Init() != WS_SUCCESS) return WS_MEMORY_E;
    s->fd = dial(h->hostname, h->port);
    int rc = WS_SOCKET_ERROR_E;
    if (s->fd < 0) goto done;

    rc = WS_MEMORY_E;
    s->ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
    if (s->ctx == NULL) goto done;
    wolfSSH_SetIORecv(s->ctx, io_recv);
    wolfSSH_SetIOSend(s->ctx, io_send);
    wolfSSH_CTX_SetPublicKeyCheck(s->ctx, check_host_key);
    wolfSSH_SetUserAuth(s->ctx, user_auth);
    // The 128 KiB default window let ^C take 1-3 s to stop a flood; 32 KiB stops it in ~70 ms
    wolfSSH_CTX_SetWindowPacketSize(s->ctx, 32768, 32768);

    s->ssh = wolfSSH_new(s->ctx);
    if (s->ssh == NULL) goto done;
    wolfSSH_SetPublicKeyCheckCtx(s->ssh, (void*)state);
    wolfSSH_SetUserAuthCtx(s->ssh, (void*)state);
    if (wolfSSH_set_fd(s->ssh, s->fd) != WS_SUCCESS || wolfSSH_SetUsername(s->ssh, h->user) != WS_SUCCESS ||
        wolfSSH_SetChannelType(s->ssh, WOLFSSH_SESSION_TERMINAL, NULL, 0) != WS_SUCCESS ||
        // EXEC keeps the terminal request TERMINAL turned on
        (s->command != NULL && wolfSSH_SetChannelType(s->ssh, WOLFSSH_SESSION_EXEC, (byte*)s->command, (word32)strlen(s->command)) != WS_SUCCESS) ||
        wolfSSH_SetTerminal(s->ssh, state->terminal_type, size->columns, size->rows, size->width_pixels, size->height_pixels, pty_modes,
                            sizeof pty_modes) != WS_SUCCESS) {
        rc = WS_BAD_ARGUMENT;
        goto done;
    }

    while (wolfSSH_connect(s->ssh) != WS_SUCCESS) {
        int e = wolfSSH_get_error(s->ssh);
        if (e != WS_WANT_READ && e != WS_WANT_WRITE) {
            rc = s->error != WS_SUCCESS ? s->error : e;
            goto done;
        }
        if (poll(&(struct pollfd){.fd = s->fd, .events = e == WS_WANT_WRITE ? POLLOUT : POLLIN}, 1, TIMEOUT_MS) <= 0) {
            rc = WS_SOCKET_ERROR_E;
            goto done;
        }
    }
    rc = WS_SUCCESS;
    wolfSSH_CTX_SetPublicKeyCheck(s->ctx, check_same_host_key);
done:
    forget_key(s);
    s->known_hosts = s->private_key = s->password = NULL;
    if (rc != WS_SUCCESS) ssh_close(state, NULL);
    return rc;
}

int ssh_read(const global_state* state, void* buffer, size_t length) {
    ssh_session* s = state->session;
    int r = wolfSSH_stream_read(s->ssh, buffer, (word32)length);
    if (r == WS_EXTDATA) r = wolfSSH_extended_data_read(s->ssh, buffer, (word32)length);
    if (r > 0) return r;
    int e = code(s, r);
    // stream_read stops reading at EOF, after which sshd sends exit-status, and while keys are exchanged
    if (e == WS_EOF || e == WS_REKEYING) e = code(s, wolfSSH_worker(s->ssh, NULL));
    switch (e) {
    case WS_SUCCESS:
    case WS_CHAN_RXD:
    case WS_EOF:
    case WS_WANT_READ:
    case WS_WANT_WRITE:
    case WS_REKEYING:
        return WS_WANT_READ;
    case WS_DISCONNECT:
        return WS_CHANNEL_CLOSED;
    default:
        return e;
    }
}

int ssh_write(const global_state* state, const void* buffer, size_t length) {
    ssh_session* s = state->session;
    int r = wolfSSH_stream_send(s->ssh, (byte*)buffer, (word32)length);
    if (r > 0) return r;
    int e = code(s, r);
    switch (e) {
    case WS_SUCCESS:
    case WS_WANT_READ:
    case WS_WANT_WRITE:
    case WS_REKEYING:
    case WS_WINDOW_FULL:
        return WS_WANT_WRITE;
    default:
        return e;
    }
}

void ssh_wait(const global_state* state, int timeout_ms) {
    poll(&(struct pollfd){.fd = state->session->fd, .events = POLLIN}, 1, timeout_ms);
}

void ssh_close(const global_state* state, int* exit_status) {
    ssh_session* s = state->session;
    if (exit_status != NULL) *exit_status = s->ssh != NULL ? wolfSSH_GetExitStatus(s->ssh) : 0;
    if (s->ssh != NULL) {
        wolfSSH_shutdown(s->ssh);
        wolfSSH_free(s->ssh);
    }
    wolfSSH_CTX_free(s->ctx);
    if (s->fd >= 0) close(s->fd);
    wolfSSH_Cleanup();
}
