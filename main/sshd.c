// SPDX-License-Identifier: GPL-3.0-or-later
#include "sshd.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/memory.h>
#include <wolfssl/wolfcrypt/random.h>

#include "config.h"

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

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool ready(int fd, short events, int timeout_ms) {
    return poll(&(struct pollfd){.fd = fd, .events = events}, 1, timeout_ms) > 0;
}

int sshd_new_host_key(char* pem, size_t size) {
    WC_RNG rng;
    ed25519_key key;
    byte der[128];
    int n = -1;
    // wolfCrypt_Init makes the DRBG's mutex where there is no static one, as on FreeRTOS: without it InitRng panics
    if (wolfSSH_Init() != WS_SUCCESS) return -1;
    if (wc_InitRng(&rng) != 0) {
        wolfSSH_Cleanup();
        return -1;
    }
    if (wc_ed25519_init(&key) == 0) {
        // The private key alone: wolfSSL's [1] public key part lacks the BIT STRING's first byte, which OpenSSL refuses
        if (wc_ed25519_make_key(&rng, ED25519_KEY_SIZE, &key) == 0 && (n = wc_Ed25519PrivateKeyToDer(&key, der, sizeof der)) > 0)
            n = wc_DerToPem(der, (word32)n, (byte*)pem, (word32)size - 1, PKCS8_PRIVATEKEY_TYPE);
        wc_ed25519_free(&key);
    }
    wc_FreeRng(&rng);
    wolfSSH_Cleanup();
    wc_ForceZero(der, sizeof der);
    if (n > 0) pem[n] = 0;
    return n;
}

int sshd_public_key(const char* pem, uint8_t out[SSHD_PUBLIC_KEY_SIZE]) {
    static const uint8_t type[] = {0, 0, 0, 11, 's', 's', 'h', '-', 'e', 'd', '2', '5', '5', '1', '9', 0, 0, 0, ED25519_PUB_KEY_SIZE};
    ed25519_key key;
    byte der[128];
    word32 index = 0, length = ED25519_PUB_KEY_SIZE;
    memcpy(out, type, sizeof type);
    int n = wc_KeyPemToDer((const byte*)pem, (int)strlen(pem), der, sizeof der, NULL);
    int rc = n > 0 ? wc_ed25519_init(&key) : -1;
    if (rc == 0) {
        rc = wc_Ed25519PrivateKeyDecode(der, &index, &key, (word32)n);
        if (rc == 0 && wc_ed25519_export_public(&key, out + sizeof type, &length) != 0) rc = wc_ed25519_make_public(&key, out + sizeof type, length);
        wc_ed25519_free(&key);
    }
    wc_ForceZero(der, sizeof der);
    return rc == 0 && length == ED25519_PUB_KEY_SIZE ? 0 : -1;
}

// Public keys only. wolfSSH checks the signature once this has said yes to the key, on the query without one as well.
// WOLFSSH_USERAUTH_SUCCESS is 0: every other path must say FAILURE.
static int user_auth(byte type, WS_UserAuthData* d, void* ctx) {
    sshd_session* s = ctx;
    if (type != WOLFSSH_USERAUTH_PUBLICKEY || s->authorized_keys == NULL || d->sf.publicKey.isCert) return WOLFSSH_USERAUTH_FAILURE;
    const WS_UserAuthData_PublicKey* pk = &d->sf.publicKey;
    if (ssh_fingerprint(s->fingerprint, sizeof s->fingerprint, pk->publicKey, pk->publicKeySz) != 0) s->fingerprint[0] = 0;
    if ((s->user == NULL || !s->user[0] || (d->usernameSz == strlen(s->user) && memcmp(d->username, s->user, d->usernameSz) == 0)) &&
        authorized_keys_check(s->authorized_keys, pk->publicKey, pk->publicKeySz))
        return WOLFSSH_USERAUTH_SUCCESS;
    s->refused = true;
    return WOLFSSH_USERAUTH_FAILURE;
}

static int auth_types(WOLFSSH* ssh, void* ctx) {
    (void)ssh, (void)ctx;
    return WOLFSSH_USERAUTH_PUBLICKEY;
}

WOLFSSH_CTX* sshd_new(const char* host_key) {
    // Never cleaned up: ssh.c's Init and Cleanup around each session then leave wolfCrypt up under a server session
    if (wolfSSH_Init() != WS_SUCCESS) return NULL;
    WOLFSSH_CTX* ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
    if (ctx == NULL) return NULL;
    wolfSSH_SetIORecv(ctx, io_recv);
    wolfSSH_SetIOSend(ctx, io_send);
    wolfSSH_SetUserAuth(ctx, user_auth);
    wolfSSH_SetUserAuthTypes(ctx, auth_types);
    if (wolfSSH_CTX_SetMaxAuthAttempts(ctx, 3) != WS_SUCCESS ||
        wolfSSH_CTX_UsePrivateKey_buffer(ctx, (const byte*)host_key, (word32)strlen(host_key), WOLFSSH_FORMAT_PEM) != WS_SUCCESS) {
        wolfSSH_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

static int code(const sshd_session* s, int r) {
    return r == WS_FATAL_ERROR ? wolfSSH_get_error(s->ssh) : r;
}

static bool transient(int e) {
    return e == WS_SUCCESS || e == WS_CHAN_RXD || e == WS_EOF || e == WS_WANT_READ || e == WS_WANT_WRITE || e == WS_REKEYING || e == WS_WINDOW_FULL;
}

int sshd_accept(WOLFSSH_CTX* ctx, sshd_session* s, int fd, int timeout_ms) {
    long deadline = now_ms() + timeout_ms;
    s->fd = fd;
    s->ssh = NULL;
    s->command[0] = s->fingerprint[0] = 0;
    s->refused = false;
    int rc = WS_SOCKET_ERROR_E;
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) != 0) goto done;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
    rc = WS_MEMORY_E;
    if ((s->ssh = wolfSSH_new(ctx)) == NULL) goto done;
    wolfSSH_SetUserAuthCtx(s->ssh, s);
    rc = WS_BAD_ARGUMENT;
    if (wolfSSH_set_fd(s->ssh, fd) != WS_SUCCESS) goto done;
    while (wolfSSH_accept(s->ssh) != WS_SUCCESS) {
        int e = wolfSSH_get_error(s->ssh);
        long left = deadline - now_ms();
        rc = e;
        if (e != WS_WANT_READ && e != WS_WANT_WRITE) goto done;
        rc = WS_SOCKET_ERROR_E;
        if (left <= 0 || !ready(fd, e == WS_WANT_WRITE ? POLLOUT : POLLIN, (int)left)) goto done;
    }
    rc = WS_SUCCESS;
    // A shell or anything else runs nothing: the caller answers an empty command
    const char* command = wolfSSH_GetSessionCommand(s->ssh);
    word32 n = wolfSSH_GetSessionCommandSz(s->ssh);
    if (wolfSSH_GetSessionType(s->ssh) == WOLFSSH_SESSION_EXEC && command != NULL && n < sizeof s->command) {
        memcpy(s->command, command, n);
        s->command[n] = 0;
    }
done:
    if (rc != WS_SUCCESS) sshd_close(s, 255);
    return rc;
}

int sshd_read(sshd_session* s, void* buffer, size_t length) {
    int r = wolfSSH_stream_read(s->ssh, buffer, (word32)length);
    if (r > 0) return r;
    int e = code(s, r);
    // stream_read stops at the client's EOF and while keys are exchanged: the worker carries on, and sees the client leave
    if (e == WS_EOF || e == WS_REKEYING) {
        int w = code(s, wolfSSH_worker(s->ssh, NULL));
        if (!transient(w)) return w;
    }
    return e == WS_EOF ? WS_EOF : transient(e) ? 0 : e;
}

void sshd_wait(const sshd_session* s, int timeout_ms) {
    ready(s->fd, POLLIN, timeout_ms);
}

int sshd_write(sshd_session* s, const void* buffer, size_t length, int timeout_ms) {
    const byte* p = buffer;
    while (length > 0) {
        int r = wolfSSH_stream_send(s->ssh, (byte*)p, (word32)length);
        if (r > 0) {
            p += r;
            length -= (size_t)r;
            continue;
        }
        int e = code(s, r);
        if (e == WS_WANT_WRITE) {
            if (!ready(s->fd, POLLOUT, timeout_ms)) return WS_SOCKET_ERROR_E;
        } else if (e == WS_WINDOW_FULL || e == WS_REKEYING || e == WS_WANT_READ) {
            // The client's window adjust, or the key exchange, is still to read
            if (!ready(s->fd, POLLIN, timeout_ms)) return WS_SOCKET_ERROR_E;
            if (!transient(e = code(s, wolfSSH_worker(s->ssh, NULL)))) return e;
        } else {
            return e;
        }
    }
    return WS_SUCCESS;
}

// EOF, the exit status and the channel's close, then the client's close, so it never sees the connection drop first; at most 2 s
void sshd_close(sshd_session* s, int exit_status) {
    if (s->ssh != NULL) {
        wolfSSH_SetExitStatus(s->ssh, (word32)exit_status);
        for (long end = now_ms() + 2000; now_ms() < end;) {
            int e = code(s, wolfSSH_shutdown(s->ssh));
            if (e != WS_WANT_READ && e != WS_WANT_WRITE && e != WS_SUCCESS) break;
            if (e != WS_SUCCESS) ready(s->fd, e == WS_WANT_WRITE ? POLLOUT : POLLIN, 100);
        }
        wolfSSH_free(s->ssh);
        s->ssh = NULL;
    }
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
}
