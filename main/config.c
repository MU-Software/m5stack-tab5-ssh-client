// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/sha256.h>

static int copy(char* out, size_t size, const cJSON* item) {
    if (item == NULL) return 0;
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= size) return -1;
    memcpy(out, item->valuestring, strlen(item->valuestring) + 1);
    return 0;
}

// network.wifi.access_points; NULL when absent, *wrong when something on the way is not an object
static cJSON* access_points(const cJSON* config, bool* wrong) {
    const cJSON* network = cJSON_GetObjectItemCaseSensitive(config, "network");
    const cJSON* wifi = cJSON_GetObjectItemCaseSensitive(network, "wifi");
    *wrong = !cJSON_IsObject(config) || (network != NULL && !cJSON_IsObject(network)) || (wifi != NULL && !cJSON_IsObject(wifi));
    return *wrong ? NULL : cJSON_GetObjectItemCaseSensitive(wifi, "access_points");
}

int config_wifi_networks(const cJSON* config, wifi_network** wifi) {
    bool wrong;
    const cJSON* list = access_points(config, &wrong);
    int n = cJSON_GetArraySize(list), err = -1;
    wifi_network* w = NULL;
    if (wrong || (list != NULL && !cJSON_IsArray(list))) goto done;
    if (n > 0 && (w = calloc((size_t)n, sizeof *w)) == NULL) goto done;
    for (int i = 0; i < n; i++) {
        const cJSON* net = cJSON_GetArrayItem(list, i);
        const cJSON* ssid = cJSON_GetObjectItemCaseSensitive(net, "ssid");
        if (ssid == NULL || copy(w[i].ssid, sizeof w[i].ssid, ssid) != 0 ||
            copy(w[i].password, sizeof w[i].password, cJSON_GetObjectItemCaseSensitive(net, "password")) != 0)
            goto done;
        w[i].next = i + 1 < n ? &w[i + 1] : NULL;
    }
    err = 0;
done:
    if (err) free(w);
    *wifi = err ? NULL : w;
    return err;
}

// parent's object `key`, made in place of anything else
static cJSON* object_in(cJSON* parent, const char* key) {
    cJSON* child = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (cJSON_IsObject(child)) return child;
    cJSON_DeleteItemFromObjectCaseSensitive(parent, key);
    return cJSON_AddObjectToObject(parent, key);
}

int config_set_wifi(cJSON* config, const char* ssid, const wifi_network* network, int move) {
    if (!cJSON_IsObject(config)) return -1;
    cJSON* wifi = object_in(object_in(config, "network"), "wifi");
    cJSON* list = cJSON_GetObjectItemCaseSensitive(wifi, "access_points");
    int at = -1;
    if (wifi == NULL) return -1;
    if (!cJSON_IsArray(list)) {
        cJSON_DeleteItemFromObjectCaseSensitive(wifi, "access_points");
        if ((list = cJSON_AddArrayToObject(wifi, "access_points")) == NULL) return -1;
    }
    for (int i = cJSON_GetArraySize(list) - 1; i >= 0; i--) {
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(list, i), "ssid");
        bool same = cJSON_IsString(name) && strcmp(name->valuestring, ssid) == 0;
        if (!same && !(network != NULL && cJSON_IsString(name) && strcmp(name->valuestring, network->ssid) == 0)) continue;
        cJSON_DeleteItemFromArray(list, i);
        if (same) at = i;
        if (at > i) at--;
    }
    if (network == NULL) return 0;
    long place = at < 0 ? 0 : (long)at + move;
    cJSON* entry = cJSON_CreateObject();
    if (entry == NULL || cJSON_AddStringToObject(entry, "ssid", network->ssid) == NULL ||
        (network->password[0] && cJSON_AddStringToObject(entry, "password", network->password) == NULL)) {
        cJSON_Delete(entry);
        return -1;
    }
    cJSON_InsertItemInArray(list, place < 0 ? 0 : place > cJSON_GetArraySize(list) ? cJSON_GetArraySize(list) : (int)place, entry);
    return 0;
}

bool config_wifi_valid(const wifi_network* network) {
    size_t ssid = strnlen(network->ssid, sizeof network->ssid), password = strnlen(network->password, sizeof network->password);
    bool hex = password == 64 && strspn(network->password, "0123456789abcdefABCDEF") == 64;
    return ssid > 0 && ssid < sizeof network->ssid && (password == 0 || (password >= 8 && password <= 63) || hex);
}

size_t config_sessions(const cJSON* list, session_record* records, size_t capacity) {
    size_t count = 0;
    const cJSON* item;
    if (!cJSON_IsArray(list)) return 0;
    cJSON_ArrayForEach(item, list) {
        const cJSON* number = cJSON_GetObjectItemCaseSensitive(item, "number");
        session_record r = {.number = cJSON_IsNumber(number) ? number->valueint : 0};
        bool taken = false;
        for (const session_record* other = records; other < records + count; other++)
            taken |= other->number == r.number;
        if (count == capacity || taken || r.number < 1 || (size_t)r.number > capacity ||
            copy(r.alias, sizeof r.alias, cJSON_GetObjectItemCaseSensitive(item, "host")) != 0 || !r.alias[0] ||
            copy(r.tmux, sizeof r.tmux, cJSON_GetObjectItemCaseSensitive(item, "tmux")) != 0 || !r.tmux[0] ||
            r.tmux[strspn(r.tmux, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")] != 0)
            continue;
        records[count++] = r;
    }
    return count;
}

cJSON* config_sessions_json(const session_record* records, size_t count) {
    cJSON* list = cJSON_CreateArray();
    for (size_t i = 0; list != NULL && i < count; i++) {
        cJSON* item = cJSON_CreateObject();
        if (item == NULL || !cJSON_AddItemToArray(list, item) || cJSON_AddNumberToObject(item, "number", records[i].number) == NULL ||
            cJSON_AddStringToObject(item, "host", records[i].alias) == NULL || cJSON_AddStringToObject(item, "tmux", records[i].tmux) == NULL) {
            cJSON_Delete(list);
            list = NULL;
        }
    }
    return list;
}

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int glob(const char* p, size_t n, const char* s) {
    if (n == 0) return *s == 0;
    if (*p == '*') return glob(p + 1, n - 1, s) || (*s != 0 && glob(p, n, s + 1));
    return *s != 0 && (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) && glob(p + 1, n - 1, s + 1);
}

static int match_list(const char* list, const char* end, const char* seps, const char* name) {
    int hit = 0;
    while (list < end) {
        while (list < end && strchr(seps, *list))
            list++;
        const char* p = list;
        while (list < end && !strchr(seps, *list))
            list++;
        int negated = p < list && *p == '!';
        if (p < list && glob(p + negated, (size_t)(list - p - negated), name)) {
            if (negated) return 0;
            hit = 1;
        }
    }
    return hit;
}

static const char* next_line(const char** text, const char** end) {
    if (**text == 0) return NULL;
    const char* line = *text;
    size_t len = strcspn(line, "\r\n");
    *text = line + len + (line[len] != 0);
    *end = line + len;
    while (line < *end && isspace((unsigned char)*line))
        line++;
    while (*end > line && isspace((unsigned char)(*end)[-1]))
        (*end)--;
    return line;
}

static const char* word(const char** p, const char* end, size_t* n) {
    while (*p < end && isspace((unsigned char)**p))
        (*p)++;
    const char* w = *p;
    while (*p < end && !isspace((unsigned char)**p))
        (*p)++;
    *n = (size_t)(*p - w);
    return w;
}

static void set_once(char* dst, size_t size, const char* value, size_t n) {
    if (dst[0] == 0 && n < size) {
        memcpy(dst, value, n);
        dst[n] = 0;
    }
}

static int keyword_is(const char* kw, size_t n, const char* name) {
    return n == strlen(name) && strncasecmp(kw, name, n) == 0;
}

static const char* next_option(const char** text, size_t* kw_len, const char** value, const char** end) {
    const char* p;
    while ((p = next_line(text, end)) != NULL && (p == *end || *p == '#')) {}
    if (p == NULL) return NULL;
    const char *kw = p, *e = *end;
    while (p < e && !isspace((unsigned char)*p) && *p != '=')
        p++;
    *kw_len = (size_t)(p - kw);
    while (p < e && (isspace((unsigned char)*p) || *p == '='))
        p++;
    // cppcheck-suppress knownConditionTrueFalse
    if (e - p >= 2 && *p == '"' && e[-1] == '"') {
        p++;
        e--;
    }
    *value = p;
    *end = e;
    return kw;
}

static void lookup(ssh_host* h, const char* text) {
    int active = 1;
    size_t kw_len;
    const char *kw, *p, *end;
    while ((kw = next_option(&text, &kw_len, &p, &end)) != NULL) {
        size_t n = (size_t)(end - p);
        if (keyword_is(kw, kw_len, "Host"))
            active = match_list(p, end, " \t", h->alias);
        else if (keyword_is(kw, kw_len, "Match"))
            active = 0;
        else if (!active)
            continue;
        else if (keyword_is(kw, kw_len, "HostName"))
            set_once(h->hostname, sizeof h->hostname, p, n);
        else if (keyword_is(kw, kw_len, "User"))
            set_once(h->user, sizeof h->user, p, n);
        else if (keyword_is(kw, kw_len, "IdentityFile"))
            set_once(h->identity_file, sizeof h->identity_file, p, n);
        else if (keyword_is(kw, kw_len, "Port") && h->port == 0) {
            long port = strtol(p, NULL, 10);
            if (port > 0 && port < 65536) h->port = (uint16_t)port;
        }
    }
}

static size_t host_aliases(const char* text, ssh_host* hosts, size_t capacity) {
    size_t count = 0, kw_len, n;
    const char *kw, *p, *end;
    while ((kw = next_option(&text, &kw_len, &p, &end)) != NULL) {
        if (!keyword_is(kw, kw_len, "Host")) continue;
        for (const char* w = word(&p, end, &n); n > 0; w = word(&p, end, &n)) {
            if (n >= sizeof hosts->alias || memchr(w, '*', n) || memchr(w, '?', n) || *w == '!') continue;
            if (hosts != NULL) {
                size_t i = 0;
                while (i < count && (strlen(hosts[i].alias) != n || memcmp(hosts[i].alias, w, n) != 0))
                    i++;
                if (i < count || count == capacity) continue;
                memcpy(hosts[count].alias, w, n);
            }
            count++;
        }
    }
    return count;
}

ssh_host* config_ssh_hosts(const char* text) {
    size_t n = host_aliases(text, NULL, 0);
    ssh_host* hosts = n > 0 ? calloc(n, sizeof *hosts) : NULL;
    if (hosts == NULL) return NULL;
    host_aliases(text, hosts, n);
    for (size_t i = 0; i < n && hosts[i].alias[0]; i++) {
        ssh_host* h = &hosts[i];
        lookup(h, text);
        if (!h->hostname[0]) memcpy(h->hostname, h->alias, sizeof h->hostname);
        if (h->port == 0) h->port = 22;
        h->next = i + 1 < n && h[1].alias[0] ? h + 1 : NULL;
    }
    return hosts;
}

// |1|salt|hash with hash = HMAC-SHA1(salt, name)
static int hashed_match(const char* entry, size_t n, const char* name) {
    const char* bar = memchr(entry + 3, '|', n - 3);
    if (bar == NULL) return 0;
    byte salt[64], hash[WC_SHA_DIGEST_SIZE], mac[WC_SHA_DIGEST_SIZE];
    word32 salt_len = sizeof salt, hash_len = sizeof hash;
    Hmac hmac;
    if (wc_HmacInit(&hmac, NULL, INVALID_DEVID) != 0) return 0;
    int ok = Base64_Decode((const byte*)entry + 3, (word32)(bar - entry - 3), salt, &salt_len) == 0 &&
             Base64_Decode((const byte*)bar + 1, (word32)(entry + n - bar - 1), hash, &hash_len) == 0 && hash_len == sizeof hash &&
             wc_HmacSetKey(&hmac, WC_SHA, salt, salt_len) == 0 && wc_HmacUpdate(&hmac, (const byte*)name, (word32)strlen(name)) == 0 &&
             wc_HmacFinal(&hmac, mac) == 0 && memcmp(mac, hash, sizeof hash) == 0;
    wc_HmacFree(&hmac);
    return ok;
}

static int hosts_match(const char* hosts, size_t n, const char* name) {
    return n > 3 && strncmp(hosts, "|1|", 3) == 0 ? hashed_match(hosts, n, name) : match_list(hosts, hosts + n, ",", name);
}

bool known_hosts_lists(const char* text, const char* name) {
    const char *p, *end;
    while ((p = next_line(&text, &end)) != NULL) {
        if (p == end || *p == '#' || *p == '@') continue;
        size_t n;
        const char* hosts = word(&p, end, &n);
        if (hosts_match(hosts, n, name)) return true;
    }
    return false;
}

host_state known_hosts_check(const char* text, const char* name, const uint8_t* key, size_t key_length) {
    if (key_length < 4 || be32(key) > key_length - 4) return HOST_UNKNOWN;
    size_t type_len = be32(key);
    host_state found = HOST_UNKNOWN;
    const char *p, *end;
    while ((p = next_line(&text, &end)) != NULL) {
        if (p == end || *p == '#' || *p == '@') continue;
        size_t hosts_len, t_len, b64_len;
        const char* hosts = word(&p, end, &hosts_len);
        const char* t = word(&p, end, &t_len);
        const char* b64 = word(&p, end, &b64_len);
        if (!hosts_match(hosts, hosts_len, name) || t_len != type_len || memcmp(t, key + 4, type_len) != 0) continue;
        byte known[2048];
        word32 known_len = sizeof known;
        if (Base64_Decode((const byte*)b64, (word32)b64_len, known, &known_len) == 0 && known_len == key_length &&
            memcmp(known, key, key_length) == 0)
            return HOST_KNOWN;
        found = HOST_CHANGED;
    }
    return found;
}

bool authorized_keys_check(const char* text, const uint8_t* key, size_t key_length) {
    if (key_length < 4 || be32(key) > key_length - 4) return false;
    const char *p, *end;
    while ((p = next_line(&text, &end)) != NULL) {
        size_t t_len, b64_len;
        const char* t = word(&p, end, &t_len);
        const char* b64 = word(&p, end, &b64_len);
        byte listed[2048];
        word32 listed_len = sizeof listed;
        if (t_len == be32(key) && memcmp(t, key + 4, t_len) == 0 && Base64_Decode((const byte*)b64, (word32)b64_len, listed, &listed_len) == 0 &&
            listed_len == key_length && memcmp(listed, key, key_length) == 0)
            return true;
    }
    return false;
}

int known_hosts_line(char* out, size_t size, const char* name, const uint8_t* key, size_t key_length) {
    int n = snprintf(out, size, "%s ", name);
    return n < 0 || (size_t)n >= size ? -1 : public_key_line(out + n, size - (size_t)n, key, key_length);
}

int public_key_line(char* out, size_t size, const uint8_t* key, size_t key_length) {
    if (key_length < 4 || be32(key) > key_length - 4) return -1;
    int n = snprintf(out, size, "%.*s ", (int)be32(key), (const char*)key + 4);
    if (n < 0 || (size_t)n + 2 >= size) return -1;
    word32 len = (word32)(size - (size_t)n - 1);
    if (Base64_Encode_NoNl(key, (word32)key_length, (byte*)out + n, &len) != 0 || (size_t)n + len + 2 > size) return -1;
    memcpy(out + n + len, "\n", 2);
    return 0;
}

int ssh_fingerprint(char* out, size_t size, const uint8_t* key, size_t key_length) {
    byte hash[WC_SHA256_DIGEST_SIZE];
    word32 len = size > 8 ? (word32)(size - 8) : 0;
    if (len == 0 || wc_Sha256Hash(key, (word32)key_length, hash) != 0 || Base64_Encode_NoNl(hash, sizeof hash, (byte*)out + 7, &len) != 0) return -1;
    memcpy(out, "SHA256:", 7);
    while (len > 0 && out[7 + len - 1] == '=')
        len--;
    out[7 + len] = 0;
    return 0;
}

int openssh_public_key(const uint8_t* container, size_t size, const uint8_t** key, size_t* key_length) {
    static const char magic[] = "openssh-key-v1";
    if (size < sizeof magic || memcmp(container, magic, sizeof magic) != 0) return -1;
    // magic, cipher, KDF, KDF options, key count, then the public key
    const uint8_t* p = container + sizeof magic;
    const uint8_t* stop = container + size;
    for (int i = 0; i < 3; i++) {
        if (stop - p < 4 || be32(p) > (uint32_t)(stop - p - 4)) return -1;
        p += 4 + be32(p);
    }
    if (stop - p < 8 || be32(p + 4) > (uint32_t)(stop - p - 8)) return -1;
    *key_length = be32(p + 4);
    *key = p + 8;
    return 0;
}
