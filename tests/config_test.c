// SPDX-License-Identifier: GPL-3.0-or-later
// Expected values come from OpenSSH: ssh-keygen -H for the hashed entry, ssh-keygen -l for the fingerprint
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wolfssl/wolfcrypt/coding.h>

#define KEY_A "AAAAC3NzaC1lZDI1NTE5AAAAICbE1A0PJ7viNG9J0Nzl0LQlsLg2rPaKlcBX3JMMIsVz"
#define KEY_B "AAAAC3NzaC1lZDI1NTE5AAAAIPUdXA3sKedRzejH2g/944l1NqLPjLvpkL2p+s3Eu4t5"

static int failures;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

static int wifi(const char* json, wifi_network** w) {
    cJSON* config = cJSON_Parse(json);
    int rc = config_wifi_networks(config, w);
    cJSON_Delete(config);
    return rc;
}

static int count(const wifi_network* w) {
    int n = 0;
    for (; w != NULL; w = w->next)
        n++;
    return n;
}

static const char ssh_config[] = "# comment\n"
                                 "Host tab5 work\n"
                                 "  HostName example.org\n"
                                 "  user = me\n"
                                 "  PORT 2222\n"
                                 "  IdentityFile \"~/.ssh/id_work\"\n"
                                 "Host printer.lan nas.lan tab5\n"
                                 "  IdentityFile ~/.ssh/id_lan\n"
                                 "Host *.lan !nas.lan\n"
                                 "  User lanuser\n"
                                 "Match all\n"
                                 "  User matched\n"
                                 "Host * b?c !x\n"
                                 "  User fallback\n";

static const ssh_host* find(const ssh_host* hosts, const char* alias) {
    while (hosts != NULL && strcmp(hosts->alias, alias) != 0)
        hosts = hosts->next;
    return hosts;
}

int main(void) {
    wifi_network* w;
    check(wifi("{\"wifi\": [{\"ssid\": \"home\", \"password\": \"secret\"}, {\"ssid\": \"cafe\"}]}", &w) == 0 && count(w) == 2 &&
              strcmp(w->ssid, "home") == 0 && strcmp(w->password, "secret") == 0 && strcmp(w->next->ssid, "cafe") == 0 && w->next->password[0] == 0,
          "wifi: networks in order, with and without a password");
    free(w);
    check(wifi("{}", &w) == 0 && w == NULL, "no wifi key, no networks");
    check(wifi("{\"wifi\": [{\"ssid\": \"0123456789abcdef0123456789abcdef\"}]}", &w) == 0 && count(w) == 1, "32-character SSID");
    free(w);
    check(wifi("{\"wifi\": [{\"ssid\": \"0123456789abcdef0123456789abcdef0\"}]}", &w) != 0 && w == NULL, "33-character SSID refused");
    check(wifi("{\"wifi\": [{\"ssid\": \"a\"}, {}]}", &w) != 0 && w == NULL, "network without ssid refuses the whole list");
    check(wifi("{\"wifi\": [{\"ssid\": 1}]}", &w) != 0, "ssid as a number refused");
    check(wifi("{\"wifi\": []}", &w) == 0 && w == NULL, "empty list, no networks");
    check(wifi("{\"wifi\": {}}", &w) != 0, "wifi as an object refused");
    check(wifi("{\"host\": ", &w) != 0 && wifi("[]", &w) != 0, "malformed or non-object JSON refused");

    ssh_host* hosts = config_ssh_hosts(ssh_config);
    check(hosts != NULL && strcmp(hosts[0].alias, "tab5") == 0 && strcmp(hosts[1].alias, "work") == 0 && strcmp(hosts[2].alias, "printer.lan") == 0 &&
              strcmp(hosts[3].alias, "nas.lan") == 0 && hosts[3].next == NULL && hosts[0].next == &hosts[1],
          "ssh_config: every Host that is not a pattern, once, in order");
    const ssh_host* h = find(hosts, "work");
    check(h && strcmp(h->hostname, "example.org") == 0 && strcmp(h->user, "me") == 0 && h->port == 2222 &&
              strcmp(h->identity_file, "~/.ssh/id_work") == 0,
          "ssh_config: case and = in keywords, quoted value");
    h = find(hosts, "printer.lan");
    check(h && strcmp(h->user, "lanuser") == 0 && h->port == 22 && strcmp(h->hostname, "printer.lan") == 0 &&
              strcmp(h->identity_file, "~/.ssh/id_lan") == 0,
          "ssh_config: wildcard, first value wins, Match skipped, defaults");
    h = find(hosts, "nas.lan");
    check(h && strcmp(h->user, "fallback") == 0, "ssh_config: negated pattern");
    free(hosts);

    uint8_t a[64], b[64];
    word32 a_len = sizeof a, b_len = sizeof b;
    Base64_Decode((const byte*)KEY_A, sizeof KEY_A - 1, a, &a_len);
    Base64_Decode((const byte*)KEY_B, sizeof KEY_B - 1, b, &b_len);
    static const char known[] = "# comment\n"
                                "@cert-authority * ssh-ed25519 " KEY_B "\n"
                                "other.org,10.0.0.1 ssh-ed25519 " KEY_B "\n"
                                "example.org ssh-rsa " KEY_B "\n"
                                "[example.org]:2222 ssh-ed25519 " KEY_B "\n"
                                "|1|02Rhrm0gvXZnXbB9H+S4pLcUTM4=|O6bq1Js/XdOabR3vj0z+eoZXqLc= ssh-ed25519 " KEY_A "\n"
                                "*.wild.org ssh-ed25519 " KEY_A "\n";
    check(known_hosts_check(known, "example.org", a, a_len) == HOST_KNOWN, "known_hosts: hashed entry");
    check(known_hosts_check(known, "x.wild.org", a, a_len) == HOST_KNOWN, "known_hosts: wildcard");
    check(known_hosts_check(known, "10.0.0.1", a, a_len) == HOST_CHANGED, "known_hosts: another key of the same type");
    check(known_hosts_check(known, "[example.org]:2222", a, a_len) == HOST_CHANGED, "known_hosts: [host]:port");
    check(known_hosts_check(known, "new.org", a, a_len) == HOST_UNKNOWN, "known_hosts: unknown host");
    check(known_hosts_check(known, "ca.org", b, b_len) == HOST_UNKNOWN, "known_hosts: markers skipped");

    char line[256], fingerprint[64];
    check(known_hosts_line(line, sizeof line, "[new.org]:2222", a, a_len) == 0 && strcmp(line, "[new.org]:2222 ssh-ed25519 " KEY_A "\n") == 0 &&
              known_hosts_check(line, "[new.org]:2222", a, a_len) == HOST_KNOWN,
          "known_hosts: line written back is found");
    check(ssh_fingerprint(fingerprint, sizeof fingerprint, a, a_len) == 0 &&
              strcmp(fingerprint, "SHA256:b8gguW9Xs3OAdre/zWaIayI0cn9KVlfdOtAxVvKatlA") == 0,
          "fingerprint as ssh-keygen -l prints it");
    return failures != 0;
}
