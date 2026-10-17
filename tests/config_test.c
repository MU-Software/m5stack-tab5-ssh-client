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

// The SSIDs of the list after config_set_wifi, "!" when it failed; a network's password shows as =PASSWORD
static const char* set_wifi(const char* json, const char* ssid, const char* new_ssid, const char* password, int move) {
    static char out[160];
    cJSON* config = cJSON_Parse(json);
    wifi_network network = {0};
    if (new_ssid != NULL)
        snprintf(network.ssid, sizeof network.ssid, "%s", new_ssid), snprintf(network.password, sizeof network.password, "%s", password);
    out[0] = 0;
    if (config_set_wifi(config, ssid, new_ssid != NULL ? &network : NULL, move) != 0) snprintf(out, sizeof out, "!");
    const cJSON* item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(config, "wifi")) {
        const cJSON* p = cJSON_GetObjectItemCaseSensitive(item, "password");
        snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s%s ", cJSON_GetObjectItemCaseSensitive(item, "ssid")->valuestring,
                 p != NULL ? "=" : "", p != NULL ? p->valuestring : "");
    }
    cJSON_Delete(config);
    return out;
}

static int valid(const char* ssid, const char* password) {
    wifi_network network = {0};
    snprintf(network.ssid, sizeof network.ssid, "%s", ssid);
    snprintf(network.password, sizeof network.password, "%s", password);
    return config_wifi_valid(&network);
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

    static const char three[] = "{\"wifi\": [{\"ssid\": \"a\", \"password\": \"aaaaaaaa\"}, {\"ssid\": \"b\"}, {\"ssid\": \"c\"}]}";
    check(strcmp(set_wifi(three, "b", "b", "bbbbbbbb", 0), "a=aaaaaaaa b=bbbbbbbb c ") == 0, "set_wifi: a new password in place");
    check(strcmp(set_wifi(three, "a", "a", "", 0), "a b c ") == 0, "set_wifi: an empty password leaves none");
    check(strcmp(set_wifi(three, "d", "d", "", 0), "d a=aaaaaaaa b c ") == 0, "set_wifi: a new network first");
    check(strcmp(set_wifi(three, "c", NULL, "", 0), "a=aaaaaaaa b ") == 0, "set_wifi: removed");
    check(strcmp(set_wifi(three, "d", NULL, "", 0), "a=aaaaaaaa b c ") == 0, "set_wifi: removing an unknown one changes nothing");
    check(strcmp(set_wifi(three, "c", "c", "", -1), "a=aaaaaaaa c b ") == 0 &&
              strcmp(set_wifi(three, "a", "a", "aaaaaaaa", -1), "a=aaaaaaaa b c ") == 0,
          "set_wifi: moved up one, not past the front");
    check(strcmp(set_wifi(three, "a", "a", "aaaaaaaa", 5), "b c a=aaaaaaaa ") == 0, "set_wifi: moved down, not past the end");
    check(strcmp(set_wifi(three, "c", "c", "", -2147483647 - 1), "c a=aaaaaaaa b ") == 0, "set_wifi: moved first");
    check(strcmp(set_wifi(three, "b", "e", "", 0), "a=aaaaaaaa e c ") == 0, "set_wifi: renamed in place");
    check(strcmp(set_wifi(three, "c", "a", "", 0), "b a ") == 0, "set_wifi: renamed onto another, which goes");
    check(strcmp(set_wifi("{\"wifi\": [{\"ssid\": \"a\"}, {\"ssid\": \"b\"}, {\"ssid\": \"a\"}]}", "a", "a", "", 0), "a b ") == 0,
          "set_wifi: duplicates go");
    check(strcmp(set_wifi("{}", "a", "a", "", 0), "a ") == 0 && strcmp(set_wifi("{\"wifi\": 1}", "a", "a", "", 0), "a ") == 0,
          "set_wifi: a list made when absent or not a list");
    check(strcmp(set_wifi("[]", "a", "a", "", 0), "!") == 0, "set_wifi: refused for a configuration that is not an object");
    check(valid("a", "") && valid("a", "12345678") && valid("0123456789abcdef0123456789abcdef", "") &&
              valid("a", "012345678901234567890123456789012345678901234567890123456789012") &&
              valid("a", "0123456789abcdefABCDEF0123456789abcdef0123456789abcdef0123456789"),
          "wifi_valid: open, 8 and 63 characters, 64 hex digits");
    check(!valid("", "") && !valid("a", "1234567") && !valid("a", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg"),
          "wifi_valid: no SSID, 7 characters, 64 not hex");

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
