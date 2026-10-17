// SPDX-License-Identifier: GPL-3.0-or-later
// usage: sshd_test KEY_FILE UNKNOWN_KEY_FILE WORK_DIR; drives the OpenSSH client in PATH against sshd.c
#include "sshd.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"

static WOLFSSH_CTX* ctx;
static char authorized_keys[4096], ssh[2048];
static const char* login = "tester";
static int listener, failures, refused;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

static void reply(sshd_session* s, const char* text) {
    sshd_write(s, text, strlen(text), 5000);
}

// What remote.c does with a command, in miniature
static int run(sshd_session* s) {
    char line[64];
    unsigned long n;
    if (strncmp(s->command, "echo ", 5) == 0) {
        snprintf(line, sizeof line, "%s\n", s->command + 5);
        reply(s, line);
        return 0;
    }
    if (sscanf(s->command, "upload %lu", &n) == 1) {
        static unsigned char data[1 << 20];
        size_t got = 0;
        for (int idle = 0; got < n && idle < 50;) {
            int r = sshd_read(s, data + got, n - got);
            if (r > 0)
                got += (size_t)r, idle = 0;
            else if (r == 0)
                sshd_wait(s, 100), idle++;
            else
                break;
        }
        unsigned long sum = 0;
        for (size_t i = 0; i < got; i++)
            sum = sum * 31 + data[i];
        snprintf(line, sizeof line, "got %zu %lx\n", got, sum & 0xffffffff);
        reply(s, line);
        return 0;
    }
    if (sscanf(s->command, "big %lu", &n) == 1) {
        static char chunk[1000];
        memset(chunk, 'x', sizeof chunk);
        for (; n >= sizeof chunk; n -= sizeof chunk)
            if (sshd_write(s, chunk, sizeof chunk, 5000) != WS_SUCCESS) return 1;
        return 0;
    }
    if (strcmp(s->command, "fail") == 0) return 3;
    reply(s, "exec only\n");
    return 1;
}

static void* serve(void* arg) {
    (void)arg;
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) continue;
        sshd_session s = {.authorized_keys = authorized_keys, .user = "tester"};
        if (sshd_accept(ctx, &s, fd, 5000) != WS_SUCCESS) {
            refused++;
            continue;
        }
        sshd_close(&s, run(&s));
    }
    return NULL;
}

// The exit status of the last command; out gets what it printed
static int client(const char* key, const char* command, const char* input, char* out, size_t size) {
    char line[4096];
    snprintf(line, sizeof line, "%s -i %s < %s %s@127.0.0.1 %s", ssh, key, input != NULL ? input : "/dev/null", login, command);
    FILE* p = popen(line, "r");
    if (p == NULL) return -1;
    size_t n = fread(out, 1, size - 1, p);
    out[n] = 0;
    int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static char* read_file(const char* path, char* text, size_t size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) return NULL;
    size_t n = fread(text, 1, size - 1, f);
    fclose(f);
    text[n] = 0;
    return text;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 4) return 2;
    char path[1024], pub[1100], out[65536], expected[64];
    static char big[1 << 20];
    snprintf(pub, sizeof pub, "%s.pub", argv[1]);
    if (read_file(pub, authorized_keys, sizeof authorized_keys) == NULL) return 2;

    char pem[256];
    uint8_t public_key[SSHD_PUBLIC_KEY_SIZE];
    check(sshd_new_host_key(pem, sizeof pem) > 0 && strstr(pem, "-----END PRIVATE KEY-----\n") != NULL && sshd_public_key(pem, public_key) == 0,
          "Ed25519 host key made as PKCS#8 PEM");
    check((ctx = sshd_new(pem)) != NULL, "and loads");
    if (ctx == NULL) return 1;

    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t address_length = sizeof address;
    if ((listener = socket(AF_INET, SOCK_STREAM, 0)) < 0 || bind(listener, (struct sockaddr*)&address, sizeof address) != 0 ||
        listen(listener, 1) != 0 || getsockname(listener, (struct sockaddr*)&address, &address_length) != 0)
        return 2;
    int port = ntohs(address.sin_port);

    char name[32], line[256];
    snprintf(name, sizeof name, "[127.0.0.1]:%d", port);
    snprintf(path, sizeof path, "%s/sshd_known_hosts", argv[3]);
    FILE* f = fopen(path, "w");
    if (f == NULL || known_hosts_line(line, sizeof line, name, public_key, sizeof public_key) != 0 || fputs(line, f) < 0 || fclose(f) != 0) return 2;
    // RekeyLimit: key exchanges in the middle of the transfers
    snprintf(ssh, sizeof ssh,
             "ssh -F /dev/null -o BatchMode=yes -o IdentitiesOnly=yes -o IdentityAgent=none -o StrictHostKeyChecking=yes "
             "-o UserKnownHostsFile=%s -o RekeyLimit=32K -o LogLevel=ERROR -p %d",
             path, port);

    pthread_t thread;
    if (pthread_create(&thread, NULL, serve, NULL) != 0) return 2;

    check(client(argv[1], "'echo hello there'", NULL, out, sizeof out) == 0 && strcmp(out, "hello there\n") == 0,
          "a command's output, from the pinned host key");
    check(client(argv[1], "fail", NULL, out, sizeof out) == 3, "its exit status");
    check(client(argv[2], "'echo no'", NULL, out, sizeof out) == 255 && out[0] == 0 && refused == 1, "a key not listed is refused");
    login = "someone";
    check(client(argv[1], "'echo no'", NULL, out, sizeof out) == 255 && refused == 2, "another user name is refused");
    login = "tester";
    check(client(argv[1], "", NULL, out, sizeof out) == 1 && strcmp(out, "exec only\n") == 0, "a shell runs nothing");

    unsigned long sum = 0;
    for (size_t i = 0; i < sizeof big; i++)
        sum = sum * 31 + (unsigned char)(big[i] = (char)(i * 7 + i / 251));
    snprintf(path, sizeof path, "%s/sshd_upload", argv[3]);
    f = fopen(path, "wb");
    if (f == NULL || fwrite(big, 1, sizeof big, f) != sizeof big || fclose(f) != 0) return 2;
    snprintf(line, sizeof line, "'upload %zu'", sizeof big);
    snprintf(expected, sizeof expected, "got %zu %lx\n", sizeof big, sum & 0xffffffff);
    check(client(argv[1], line, path, out, sizeof out) == 0 && strcmp(out, expected) == 0, "1 MiB from stdin, intact");
    check(client(argv[1], "'big 4000000' | wc -c", NULL, out, sizeof out) == 0 && atol(out) == 4000000, "4 MB out, past the client's window");
    unlink(path);
    return failures != 0;
}
